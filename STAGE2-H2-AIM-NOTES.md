# Stage 2A — Halo 2 stock aim-assist/melee diagnostic

## Baseline

- Runtime/source foundation: accepted MCC VR Alpha 0.3.5 `1939eabc21c1607ef93ccaec97de004271d70091`.
- Stage 1 C50 Halo 4 fail-closed reticle safety remains intact.
- No Halo 2/H2A stereo, hand, two-hand, HUD-replay groundwork, pause, performance, or lifecycle code is intentionally changed.

## Report being tested

After Stage 1, the user/testers reported that every title injects and renders correctly in VR, but Halo 2 appears to retain controller aim assist and melee/lunge feels wrong.

The existing 1939 source runs H2EK-proven `unit_update_aiming` first and then overwrites only the local player's desired/current native aim vectors (`unit +0x168` and `+0x174`) with the VR controller sight line. A plausible diagnostic hypothesis is that the stock update can leave target-acquisition/assist state inconsistent with the final VR-owned sight line. This is a hypothesis to test, not a claim that the H2EK evidence already proves the exact melee state owner.

## Source change

`src/dll/halo2_observer_6dof.cpp` now computes `directVrAim` before the stock update:

- if VR direct aim owns the local player, stock `unit_update_aiming` is skipped and the existing H2EK-proven controller-vector write body runs;
- if VR direct aim does **not** own the update, stock `unit_update_aiming` runs exactly as before.

This is intentionally a one-behavior diagnostic.

## Test DLL implementation

The supplied Linux environment cannot rebuild the Windows DLL with MSVC, so the Stage 2A DLL is a post-link binary equivalent of the source change, based directly on the Stage 1 DLL.

It does **not** replace the stock `call rsi` unconditionally. Instead it:

1. Adds a 130-byte RX section named `.h2aa` at RVA `0x2EA000`.
2. Replaces the 10-byte setup block at VA `0x180095641` with a jump to `.h2aa`.
3. Replays the displaced setup bytes in `.h2aa`.
4. Evaluates the same VR ownership predicates used by the source (`Game_Halo2ControllerAimActive` plus the `DirectWeaponAimArmed` state).
5. If direct VR aim is **false**, jumps back to VA `0x18009564B`; the original test/branch and original `call rsi` at `0x180095650` remain byte-for-byte intact and execute in their original exception-handled location.
6. If direct VR aim is **true**, initializes the existing result/applied locals and enters the already-compiled controller-vector body at VA `0x1800956EB`, skipping only the stock aim update.

The PE header's section count, `SizeOfCode`, and `SizeOfImage` are updated coherently. The new section uses only RIP-relative/rel32 addressing, so it adds no absolute base relocations.

Reproduction script: `tools/build_stage2_h2aim_trampoline.py`.

## Explicitly NOT changed in Stage 2A

- Halo 4 Promethean/muzzle-effect rerooting: still missing from Stage 1. The old V6 layer is sourceless/address-sensitive and is being handled separately.
- Halo 2 Classic vs Anniversary weapon placement: no calibration values are changed yet. The current source indeed shares global gun placement settings, and per-title/per-mode calibration is planned as a separate source change.
- Halo 2 HUD/native reticle work: not touched.
- Halo 4 native animated reticle work: not touched; C50 fail-closed safety remains.

## What to test

1. Halo 2 Anniversary: normal shooting/aiming, multiple weapons, melee at close and lunge range.
2. Halo 2 Classic: same test, and note whether melee behavior differs from Anniversary.
3. Look specifically for whether the reported aim-assist pull/snapping is reduced or gone.
4. Confirm bullets still follow the controller sight.
5. Confirm hands/two-hand grip, Classic/Anniversary switching, pause, and stereo remain normal.
6. Brief H3/ODST/Reach/H4 injection regression.

If melee becomes worse or bullets stop matching the controller sight, reject this diagnostic and return to Stage 1; do not stack another H2 aim experiment on top of it.
