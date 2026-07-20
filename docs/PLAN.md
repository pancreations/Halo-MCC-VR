# Development plan

CURRENT-STATE.md is the authority for what works and what must not be repeated.

## Gate 1: lock the recovered baseline

- Preserve 330a568 and recovery/best-working-20260719-1300 as the protected rollback baseline. The active feature checkpoint is 1fc56c8 on feature/resolution-scale; it includes the headset-confirmed HUD aspect and class-gated crosshair fixes plus the resolution presets.
- The broad cleanup at 42a1276 caused a fatal level-transition regression and was reverted at ddfe109. The runtime tag-table reticle classifier at f0d5a88 separately caused a black headset view and was narrowly reverted at 7fdf019. Neither failed path is a fallback.
- Headset regression test: level load, stereo, head tracking, movement, firing, reload, melee, grenade, VR reticle, absence of native class-2 crosshairs, native HUD, and both arms.
- Weapon check: spawn shotgun first, then assault rifle and pistol. Confirm the left hand stays under controller control before and after switching or melee.
- Remove dormant runtime code only one independently understood path at a time, on its own commit and headset build. If anything regresses, return to a named headset-confirmed checkpoint rather than mixing partial experiments.

## Gate 2: weapon coverage

Create a repeatable weapon matrix for rifles, pistols, shotguns, dual wield, heavy weapons, turrets, reloads, melee, grenades, pickup/drop, death/respawn, and level transitions. Fix only headset-reproduced failures. No generic "all weapons" claim until the matrix passes.

## Gate 3: gameplay completeness

Validate scopes, vehicles, turrets, equipment, cutscenes, checkpoints, long sessions, and co-op behavior. Each fix needs a unique signature, safe failure mode, and isolated Git checkpoint.

## Gate 4: ODST

Verify engine signatures and gameplay end to end in ODST. Treat any binary difference as a separate target; do not assume Halo 3 offsets or behavior.

## Gate 5: release quality

- Run the resolution-preset matrix on Quest 3 and PSVR2 with Toolkit scaling disabled: Potato 50%, Low 67%, Medium 80%, High 100%, and Ultra 110%. Low has passed; the other four tiers are pending.
- Record GPU frame time and image quality at each tier, including an RTX 2070 Super-class machine. Confirm every tier keeps the complete eye image inside the unchanged full-size OpenXR projection after the required game restart.
- Passed 2026-07-19: portable alpha export plus first clean install and launch on a separate RTX 4060 laptop.
- Still test reinstall, update, uninstall, and non-default Steam library discovery on a separate PC.
- Add friendly signature/version failure messages.
- Freeze a release candidate, publish hashes, and run the complete acceptance matrix before packaging.

## Current non-goals

- Do not add an FOV slider for the current compatibility/performance pass.
- Do not bundle FSR. The observed OpenXR Toolkit FSR configuration produced a tiled/overlapping VR View, and the user selected the simpler internal resolution-scale path. External upscalers remain optional and unsupported.
- Do not replace the engine scripting-class-2 crosshair gate with per-weapon runtime ids or tag-table dereferences.

## Change acceptance

A task passes only when Release builds, deploy verifies every affected binary byte-for-byte (DLL and launcher for resolution/launch changes), the exact build is identified in the log, and the headset result matches the requested behavior without a new crash, black screen, control regression, or performance regression.
