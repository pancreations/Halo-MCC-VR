# Development plan

CURRENT-STATE.md is the authority for what works and what must not be repeated.

## Gate 1: lock the recovered baseline

- Build and deploy the cleanup branch.
- Headset regression test: level load, stereo, head tracking, movement, firing, reload, melee, grenade, reticle, and both arms.
- Weapon check: spawn shotgun first, then assault rifle and pistol. Confirm the left hand stays under controller control before and after switching or melee.
- If anything regresses, return to 330a568 and compare one subsystem at a time.

## Gate 2: weapon coverage

Create a repeatable weapon matrix for rifles, pistols, shotguns, dual wield, heavy weapons, turrets, reloads, melee, grenades, pickup/drop, death/respawn, and level transitions. Fix only headset-reproduced failures. No generic "all weapons" claim until the matrix passes.

## Gate 3: gameplay completeness

Validate scopes, vehicles, turrets, equipment, cutscenes, checkpoints, long sessions, and co-op behavior. Each fix needs a unique signature, safe failure mode, and isolated Git checkpoint.

## Gate 4: ODST

Verify engine signatures and gameplay end to end in ODST. Treat any binary difference as a separate target; do not assume Halo 3 offsets or behavior.

## Gate 5: release quality

- Test performance at multiple OpenXR resolutions, including an RTX 2070 Super-class machine.
- Test clean install, reinstall, update, uninstall, and non-default Steam library discovery on a friend's PC.
- Add friendly signature/version failure messages.
- Freeze a release candidate, publish hashes, and run the complete acceptance matrix before packaging.

## Change acceptance

A task passes only when Release builds, deploy verifies byte-for-byte, the exact build is identified in the log, and the headset result matches the requested behavior without a new crash, black screen, control regression, or performance regression.