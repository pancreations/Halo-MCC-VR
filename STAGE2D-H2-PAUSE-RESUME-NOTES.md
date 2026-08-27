# Stage 2D — Halo 2 native resume synchronization (C-H2-73)

## Baseline

Stage 2D is based byte-for-byte on the accepted Stage 2C DLL before the one
new H2 resume interposition. It retains:

- upstream accepted 1939eabc Halo 2 / Anniversary stereo and 6DOF
- Stage 2A H2 aim/melee diagnostic
- Stage 2B H2 pause ownership fix
- Stage 2C head-locked native H2 pause-screen delivery
- C50 Halo 4 fail-closed reticle safety

## Headset evidence that motivated C-H2-73

Stage 2C proved the pause screen itself is fixed. Y+B switches the headset to
head-locked 2D and the native Halo 2 pause menu is visible. Selecting Resume in
the native menu, however, produces no second Y+B/Start edge, so the shared VR
presentation remains head-locked until Y+B is pressed again. The next Y+B only
changes the mod's pause target; once that happens the existing H2 synchronous
stereo core immediately resumes valid exact-current-serial pairs.

## C-H2-73 policy

Halo 2 still has no independently proven native pause boolean in this branch.
Do not treat A, B, or any menu button as Resume because those buttons can be
used inside Settings and other pause submenus.

Instead, reuse the already H2EK-proven `game_time_globals` current-tick field:

1. Only monitor while Halo 2's *displayed* pause presentation is active.
2. Require four repeated samples of the same initialized native tick after the
   baseline sample. This proves the simulation clock is actually frozen.
3. After that proof, require two distinct native tick advances.
4. Then set H2 presentation-ready false and request stereo 3D through the
   existing shared comfort-fade transition.
5. Any invalid/uninitialized sample, leaving H2, or leaving the displayed pause
   presentation resets the evidence state.

This is intentionally a resume fallback, not a declaration that H2 now has an
authoritative pause flag. `Game_HasAuthoritativePauseState()` remains unchanged,
so Y+B still owns pause-entry injection.

## Source implementation

- `src/common/halo2_render_logic.h`
  - `Halo2PauseResumeClock`
  - four-sample freeze proof
  - two-distinct-tick resume proof
- `src/dll/halo2_cold_observation.h/.cpp`
  - exposes a read-only wrapper around the existing coherent H2EK game-time
    sample
- `src/dll/game.cpp`
  - observes the clock only while H2's displayed pause presentation is active
  - requests stereo only after the pure state machine proves resume
- `tests/core_tests.cpp`
  - covers freeze -> two changes, moving-without-freeze, and invalid-sample reset

The pure helper was additionally compiled and executed under Linux as a smoke
test because the current environment does not contain the Windows SDK needed to
rebuild the DLL itself.

## Exact Stage 2D post-link test DLL

The matching runtime test DLL is reproduced from the exact Stage 2C DLL by
`tools/build_stage2d_h2_pause_resume_clock.py`.

Stage 2C input SHA-256:
`a2460f279783df5d9e30feac23cc9fb22a6b66a79487a707fb17ef9c05d05f0f`

Stage 2D output SHA-256:
`00451d8d5e83a5514fade7ba719e13f8229cd8a6be657329e41a1d5704016ccc`

Binary scope:

- existing `.rdata`, `.data`, `.pdata`, `.fptable`, `.rsrc`, `.reloc`, `.h2aa`,
  and `.h2pm` section contents are byte-identical to Stage 2C
- existing `.text` differs at exactly one contiguous 17-byte H2-only block,
  RVA `0x42CB3..0x42CC3`
- new `.h2pr`: RX C-H2-73 helper + diagnostic string
- new `.h2pd`: 8-byte RW clock state
- the helper calls the already-compiled coherent game-time reader at
  `0x180088FB0`; no retail halo2.dll offset is guessed or newly patched
- on proven resume it logs:
  `Halo 2 pause presentation: native game-time clock resumed; restoring stereo 3D`

## Test focus

1. Halo 2 Anniversary gameplay -> Y+B.
2. Confirm native pause menu remains visible in VR.
3. Navigate within pause/settings briefly: VR must remain head-locked.
4. Select Resume with the normal menu control (do NOT press Y+B again).
5. Expected: a short comfort fade automatically returns to stereo 3D.
6. Repeat several times and repeat in Classic graphics.
7. If it fails, preserve the log. Search for the C-H2-73 diagnostic above.

## Not changed

- H2 Classic/Anniversary gun presentation calibration
- H2 native center-reticle suppression / authored reticle capture
- H4 Promethean/muzzle effect restoration
- H4 native animated reticle work
