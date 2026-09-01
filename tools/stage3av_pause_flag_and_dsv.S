// Stage 3AV - Halo 4 menu-resume pause exit + capture depth-target fix.
//
// Measured, 2026-08-28 09:10 Steam log (Stage 3AU installed, hash-verified):
//
//   09:10:18.730  controller edge: Y
//   09:10:18.751  controller edge: B
//   09:10:18.751  pause fallback: injecting Start, presentation
//                 control=edge fallback, target=head-locked 2D
//   09:10:18.956  pause transition: presentation switched to head-locked 2D
//   09:10:20.132  controller edge: A          <- "Resume Game" in the menu
//   ...           NO pause exit line ever fires; presentation stays
//                 head-locked 2D while "fps 112 (stereo on)" - black screen.
//
// The only Halo 4 pause-exit detection was Stage 3X's raw B edge. Resuming
// through the pause menu itself (A on "Resume Game") produces no B edge, so
// nothing ever restores stereo. Halo 2 solved this exact problem (C-H2-73)
// by proving the native game clock froze during the pause and resumed after
// it - "do not guess from A/B".
//
// halo4.dll carries a `game_paused` entry (boolean, type 5) in the same
// debug-var table this mod already resolves `enable_first_person_squish`
// from, BY NAME, through the DLL's own FindDebugVarSlot. Its value pointer
// is null on disk; if the engine fills it at boot the resolver returns it,
// otherwise null and everything below stays inert.
//
//   * stage3av_poll_chain      (splice: the Stage 3X poll-gate call edge)
//       While the loader-pinned halo4.dll reference is live, resolve
//       `game_paused` exactly once; clear the cached pointer when the module
//       reference drops. Then continue into the untouched Stage 3X gate.
//   * stage3av_pause_monitor   (splice: the Stage 3X heartbeat jump edge)
//       On every validated H4 camera callback: while the head-locked pause
//       target is active, first require the engine flag to actually read
//       nonzero (polarity proof, per-pause), then treat 8 consecutive zero
//       reads as the native resume and restore stereo exactly the way the
//       Stage 3X B-edge wrapper does, including the same resume grace stamp.
//
// Second fix - why every gameplay capture measured `alpha 0 rgb 0`:
// OMSetRenderTargetsHook forwards the game's DEPTH-STENCIL view unchanged
// (d3d11_hook.cpp: `g_origOMSetRenderTargets(context, count, redirected,
// dsv)`). During an active Halo 4 authored-reticle capture the reroute
// substitutes the private 512x512 target for the learned full-size scene
// target; if the same engine bind carries its full-size DSV, D3D11 sees an
// RTV/DSV dimension mismatch and silently drops every subsequent draw -
// matching "0 write failures" with zero ink at BOTH capture framings
// (2048 in the 08:38 log, 7572 in the 09:10 log), while the one capture that
// ever landed ink (art 4263) came from a pass that flushed before any
// depth-carrying rebind. The capture's own opening bind already uses a null
// DSV (BeginAuthoredReticleCaptureInternal).
//
//   * stage3av_dsv_gate        (splice: the hook's argument-restore tail)
//       Only when the reroute actually rewrote the bind (al != 0) AND the
//       capture is active AND the active title is Halo 4, the forwarded DSV
//       becomes null; a one-time log line proves the case actually occurs.
//       Every other bind forwards byte-identically.

.intel_syntax noprefix
.text

.global stage3av_data
.global stage3av_dsv_gate
.global stage3av_poll_chain
.global stage3av_pause_monitor

// --------------------------------------------------------------------------
// State. All bytes are zero at build time; the builder proves the whole
// region was free beforehand.
// --------------------------------------------------------------------------
stage3av_data:
s3av_gp_slot:                     // 0 = unresolved, 1 = tried & absent,
    .quad 0                       // else pointer to halo4's boolean
s3av_validated:
    .byte 0                       // engine flag read nonzero this pause
s3av_zero_run:
    .byte 0                       // consecutive zero reads while validated
s3av_logged_dsv:
    .byte 0                       // one-time DSV proof line emitted
    .byte 0
s3av_dsv_nulls:
    .long 0                       // diagnostic: depth targets nulled

// --------------------------------------------------------------------------
// Called from OMSetRenderTargetsHook+0x55 (0xDEB5), displacing the three
// argument-restore moves. al still holds VR_RedirectRenderTargets' bool.
// rbx/rsi/rdi are the hook's saved count/dsv/context; r8 is already the
// redirected (or stock) RTV array. Returns into the ff15 original call.
// --------------------------------------------------------------------------
stage3av_dsv_gate:
    mov r9, rsi                   // displaced: dsv
    mov edx, ebx                  // displaced: count
    mov rcx, rdi                  // displaced: context
    test al, al                   // reroute actually rewrote the bind?
    je .dsv_done
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0
    je .dsv_done
    cmp BYTE PTR [rip + ACTIVE_TITLE_RVA], 4
    jne .dsv_done
    test r9, r9                   // engine bind carried no depth: unchanged
    jz .dsv_done
    xor r9d, r9d
    inc DWORD PTR [rip + s3av_dsv_nulls]
    cmp BYTE PTR [rip + s3av_logged_dsv], 0
    jne .dsv_done
    mov BYTE PTR [rip + s3av_logged_dsv], 1
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x28
    lea rcx, [rip + s3av_msg_dsv]
    call LOG_RVA
    add rsp, 0x28
    pop r9
    pop r8
    pop rdx
    pop rcx
.dsv_done:
    ret

// --------------------------------------------------------------------------
// Called instead of the Stage 3X poll gate at 0x8431C; tail-jumps into it so
// its own ret returns to the untouched Poll logic. CL carries the live H4
// admission decision and must survive (same contract the 3X gate documents).
// --------------------------------------------------------------------------
stage3av_poll_chain:
    cmp QWORD PTR [rip + H4_MODULE_REF_RVA], 0
    jne .pc_installed
    // Module reference dropped: forget the pointer (it targeted the unloaded
    // image) and rearm so the next level load resolves again.
    xor eax, eax
    mov QWORD PTR [rip + s3av_gp_slot], rax
    mov BYTE PTR [rip + s3av_validated], al
    mov BYTE PTR [rip + s3av_zero_run], al
    jmp .pc_chain
.pc_installed:
    cmp QWORD PTR [rip + s3av_gp_slot], 0
    jne .pc_chain                 // resolved or known-absent: nothing to do
    sub rsp, 0x38
    mov BYTE PTR [rsp+0x30], cl
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    mov edx, 0x04A3F000           // kHalo4RetailImageSize, guarded at 0x5993E
    lea r8, [rip + s3av_str_game_paused]
    mov r9d, 5                    // boolean, same type the squish resolve uses
    call FINDDEBUGVAR_RVA
    test rax, rax
    jz .pc_absent
    mov QWORD PTR [rip + s3av_gp_slot], rax
    mov rdx, rax
    sub rdx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    lea rcx, [rip + s3av_msg_gp_found]
    call LOG_RVA
    jmp .pc_resolved
.pc_absent:
    mov QWORD PTR [rip + s3av_gp_slot], 1
    lea rcx, [rip + s3av_msg_gp_absent]
    call LOG_RVA
.pc_resolved:
    mov cl, BYTE PTR [rsp+0x30]
    add rsp, 0x38
.pc_chain:
    jmp S3X_POLL_GATE_RVA

// --------------------------------------------------------------------------
// Jumped to from the Stage 3X heartbeat splice at 0x56CA7; tail-jumps into
// the untouched 3X bridge. Runs once per validated H4 camera callback. The
// hot path is three loads and compares - deterministic, allocation-free.
// The 3X bridge itself preserves rdx/r8/r9/r11 and clobbers rax/rcx/r10, so
// this stub keeps the same discipline (the rare restore branch saves what
// the bridge saves).
// --------------------------------------------------------------------------
stage3av_pause_monitor:
    mov rax, QWORD PTR [rip + s3av_gp_slot]
    cmp rax, 1
    jbe .pm_chain                 // unresolved or absent
    cmp QWORD PTR [rip + H4_MODULE_REF_RVA], 0
    je .pm_chain
    mov al, BYTE PTR [rax]        // halo4's own game_paused
    cmp BYTE PTR [rip + PAUSE_TARGET_BYTE_RVA], 0
    je .pm_idle                   // not presenting the head-locked pause
    test al, al
    jz .pm_maybe_exit
    // Engine reports paused while our pause screen is up: polarity proven
    // for THIS pause; arm the exit detector.
    mov BYTE PTR [rip + s3av_validated], 1
    mov BYTE PTR [rip + s3av_zero_run], 0
    jmp .pm_chain
.pm_maybe_exit:
    cmp BYTE PTR [rip + s3av_validated], 0
    je .pm_chain
    mov al, BYTE PTR [rip + s3av_zero_run]
    inc al
    mov BYTE PTR [rip + s3av_zero_run], al
    cmp al, 8
    jb .pm_chain
    // Native resume proven: the engine unpaused and stayed unpaused for 8
    // validated camera callbacks while the pause screen was still presented.
    // Restore stereo exactly like the Stage 3X B-edge wrapper: request the
    // stereo target and stamp the same resume grace the pause-toggle gate
    // and the 3X poll gate already honour.
    mov BYTE PTR [rip + s3av_validated], 0
    mov BYTE PTR [rip + s3av_zero_run], 0
    sub rsp, 0x40
    mov QWORD PTR [rsp+0x20], rdx
    mov QWORD PTR [rsp+0x28], r8
    mov QWORD PTR [rsp+0x30], r9
    mov QWORD PTR [rsp+0x38], r11
    xor ecx, ecx
    call REQUEST_PAUSE_RVA
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    mov QWORD PTR [rip + PAUSE_GRACE_RVA], rax
    lea rcx, [rip + s3av_msg_resume]
    call LOG_RVA
    mov rdx, QWORD PTR [rsp+0x20]
    mov r8,  QWORD PTR [rsp+0x28]
    mov r9,  QWORD PTR [rsp+0x30]
    mov r11, QWORD PTR [rsp+0x38]
    add rsp, 0x40
    jmp .pm_chain
.pm_idle:
    cmp BYTE PTR [rip + s3av_validated], 0
    je .pm_chain
    mov BYTE PTR [rip + s3av_validated], 0
    mov BYTE PTR [rip + s3av_zero_run], 0
.pm_chain:
    jmp S3X_HEARTBEAT_BRIDGE_RVA

// --------------------------------------------------------------------------
s3av_str_game_paused:
    .asciz "game_paused"
s3av_msg_gp_found:
    .asciz "S3AV: halo4 game_paused resolved at halo4+0x%llX; menu-resume pause exit armed"
s3av_msg_gp_absent:
    .asciz "S3AV: halo4 game_paused debug var not live in this build; pause exit stays edge-driven"
s3av_msg_resume:
    .asciz "Halo 4 pause presentation: native game_paused cleared; restoring stereo 3D"
s3av_msg_dsv:
    .asciz "S3AV: H4 capture rebind carried a full-size depth target; forwarding it null so rerouted CUI draws can land"
