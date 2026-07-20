# Development plan

CURRENT-STATE.md is the authority for what works and what must not be repeated.
This file only sequences the work that is still open. If an item here disagrees
with CURRENT-STATE.md, CURRENT-STATE.md wins and this file is the one to fix.

## Standing baseline rules

- Current best-working checkpoint: `recovery/best-working-20260720`, also the
  `master` tip. It carries dual wield, the smooth-turn jitter fix, and the
  optional floating-hands mode. Start new work from a named branch off it.
- Preserve 330a568 and recovery/best-working-20260719-1300 as protected rollback
  points. Do not rewrite or delete recovery branches.
- The broad cleanup at 42a1276 caused a fatal level-transition regression and was
  reverted at ddfe109. The runtime tag-table reticle classifier at f0d5a88
  separately caused a black headset view and was narrowly reverted at 7fdf019.
  Neither failed path is a fallback.
- Remove dormant runtime code only one independently understood path at a time,
  on its own commit and headset build. If anything regresses, return to a named
  headset-confirmed checkpoint rather than mixing partial experiments.
- Headset regression test for any risky build: level load, stereo, head tracking,
  movement, firing, reload, melee, grenade, VR reticle, absence of native class-2
  crosshairs, native HUD, and both arms.

## Gate 2: weapon coverage

Create a repeatable weapon matrix for rifles, pistols, shotguns, dual wield,
heavy weapons, turrets, reloads, melee, grenades, pickup/drop, death/respawn, and
level transitions. Fix only headset-reproduced failures. No generic "all weapons"
claim until the matrix passes.

The dual-wield ownership milestone passed in `c2e6a27` and is headset-confirmed
nearly perfect for the tested pairing. Keep the existing single-weapon
support-hand behavior and centered two-hand barrel alignment as permanent
regression checks. Broader weapon-pair coverage remains part of the matrix.

Spawn shotgun first, then assault rifle and pistol. Confirm the left hand stays
under controller control before and after switching or melee.

## Gate 3: gameplay completeness

Validate scopes, vehicles, turrets, equipment, cutscenes, checkpoints, long
sessions, and co-op behavior. Each fix needs a unique signature, safe failure
mode, and isolated Git checkpoint.

Carried-over open items with no recorded headset result yet:

- The focused Pause -> Resume -> Restart Level -> 3D acceptance sequence. The
  native pause-state build is the current checkpoint, but this sequence has not
  been explicitly recorded as passing.
- Bullet origin. Halo spawns first-person bullets from the head camera by
  design, and projectile direction is already controller-aligned. Do not claim a
  muzzle-origin hook exists. The fire function is not reachable from the static
  binary (allocator pointers), so this needs a runtime probe to locate a fire
  boundary, not an offline signature hunt. Composed-bone edits are a known
  failure: they spun the hand and sent shots sideways.

## Gate 4: ODST

Not a quick port. The measured survey in CURRENT-STATE.md found 8 of 20
production signatures match ODST byte-for-byte while 12 fail, including the
load-bearing stereo path, because structure layouts shifted (the gun/overlay
camera stride is 0x2820 in Halo 3 and 0x2810 in ODST).

- The first ODST step is not a code change: confirm the eight matching signatures
  land in the same functions, then run a live scan for the shifted camera struct.
- Repairing the 12 signatures is the cheap half. Re-deriving the camera, view,
  and first-person struct layouts on the ODST binary and re-validating VRIK and
  the palette path in the headset is the expensive half.
- Do not reuse any Halo 3 struct offset in ODST without confirming it on the ODST
  binary.
- Shipping safety holds today: ODST is registered with `runtimeSupported=false`
  and hooks are gated on `GameTitle::Halo3`, so ODST loads stock and untouched.

## Gate 5: release quality

- Run the resolution-preset matrix on Quest 3 and PSVR2 with Toolkit scaling
  disabled: Potato 50%, Low 67%, Medium 80%, High 100%, Ultra 110%, and Keith
  David 150%. Low has passed; the other five tiers are pending. Keith David
  renders 4368x3150 and has never been run in a headset.
- Record GPU frame time and image quality at each tier, including an RTX 2070
  Super-class machine. Confirm every tier keeps the complete eye image inside the
  unchanged full-size OpenXR projection after the required game restart.
- Passed 2026-07-19: portable alpha export plus first clean install and launch on
  a separate RTX 4060 laptop.
- The installer now asks for a picture-quality tier and supports installing over
  a previous version, preserving other tuned settings. This is verified offline
  against a fake game tree only; a real tester must still exercise reinstall,
  update-over-old-version, uninstall, and non-default Steam-library discovery on
  a separate PC.
- Add friendly signature/version failure messages.
- Freeze a release candidate, publish hashes, and run the complete acceptance
  matrix before packaging.

## Current non-goals and closed decisions

- Do not add an FOV slider for the current compatibility/performance pass.
- Do not bundle FSR. The observed OpenXR Toolkit FSR configuration produced a
  tiled/overlapping VR View, and the user selected the simpler internal
  resolution-scale path. External upscalers remain optional and unsupported.
- Do not replace the engine scripting-class-2 crosshair gate with per-weapon
  runtime ids or tag-table dereferences.
- Leg/hip hiding: CLOSED by user decision 2026-07-20 ("we can keep the legs its
  ok"). The visible legs are `fp_body.render_model`, which does not flow through
  `FpVisiblePaletteHook`, so floating-hands cannot reach them. Do not build a
  leg-hider unless the user reopens it.
- HUD: CLOSED by user directive. The native full-size HUD plus the 0x2EDF24
  reticle kill is the accepted final state. Do not iterate on HUD unless asked.
- Do not auto-force V-Sync off or override MCC video settings. Testers set
  V-Sync/framerate limit themselves; point "capped at 60" reports to MCC
  Settings -> Video.
- Full-body VRIK stays gated. The first-person skeleton is arms only (no
  pelvis/leg/torso bones), so full body needs a proven render path before any IK
  work, not more bone-writing.

## Change acceptance

A task passes only when Release builds, deploy verifies every affected binary
byte-for-byte (DLL and launcher for resolution/launch changes), the exact build
is identified in the log, and the headset result matches the requested behavior
without a new crash, black screen, control regression, or performance regression.
