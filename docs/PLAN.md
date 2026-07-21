# Development plan

CURRENT-STATE.md is the authority for what works and what must not be repeated.
This file only sequences the work that is still open. If an item here disagrees
with CURRENT-STATE.md, CURRENT-STATE.md wins and this file is the one to fix.

## Standing baseline rules

- Frozen Halo 3 alpha baseline: `v0.1.3-alpha` at `6f8236b`; its runtime source
  is the headset-confirmed crosshair-fallback build at `bb4bb6f`. Preserve
  `recovery/best-working-20260721-crosshair` at `c58db2e`. Start ODST work from
  a clean named branch off the frozen alpha line and do not mix it into the Halo
  3 release branch.
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

Gates 2 and 3 remain Halo 3 regression/release coverage, but the user considers
the current Halo 3 experience strong enough that they no longer block starting
ODST. Shared-system changes must still pass the Halo 3 regression list.

## Active Gate 4: ODST

Not a quick port. The measured survey in CURRENT-STATE.md found 8 of 20
production signatures match ODST byte-for-byte while 12 fail, including the
load-bearing stereo path, because structure layouts shifted (the gun/overlay
camera stride is 0x2820 in Halo 3 and 0x2810 in ODST).

- User decision 2026-07-21: begin ODST now while preserving Halo 3 as the
  protected baseline. ODST remains an isolated, gated port rather than an
  extension of the Halo 3 hook assumptions.
- Completed first code checkpoint: the one universal `halomccvr.cfg` and F1
  menu are organized for all titles while preserving every key/value and the
  launcher's `resolution_scale` contract. The Halo 3 headset result was a
  positive initial regression check, with the complete matrix still required
  before merge or public ODST support.
- Config values are universal player preferences. Verified per-title camera,
  weapon, skeleton, HUD, and engine calibration lives in the title adapter; do
  not expose a second ODST config file or force the user to retune when changing
  games. Unsupported capabilities preserve their saved values.
- Completed evidence gates: all eight matching signatures land in equivalent
  functions; the twelve failed production roles have unique ODST patterns; and
  the `0x90` compact camera, `0xC0` derived blocks, prepared-view front/nested
  structures, `+0x27FC` user index, and `0x2810` stride are proven. Read-only
  stock captures covered movement, zoom, death/respawn, unload/reload,
  cutscenes, and vehicle entry/exit.
- Desk-side implementation candidate complete: the default-OFF
  `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP` option enables an isolated private path
  only when explicitly configured ON. It requires the exact ODST retail build,
  ten unique title-specific signatures, the derived four-slot array, and all
  proven layout/single-user invariants before atomically installing the four
  camera-copy, inner-render, FP-rebuild, and FP-driver hooks. Runtime failure,
  unload, or title exit falls back and rearms fail closed.
- Do not call the monolithic Halo 3 installer for ODST. Controls, aim, reticle,
  HUD/VISR, scopes, pause, brightness, motion blur, weapons, bones, arms, VRIK,
  and gameplay patches remain later isolated gates.
- Do not reuse any Halo 3 struct offset in ODST without confirming it on the ODST
  binary.
- Shipping safety holds today: ODST remains `runtimeSupported=false` with
  capabilities `None`, and an option-OFF build gives it no hook plan. The
  normal deploy and alpha-export scripts require an exact OFF cache and reject
  a private ON build. Do not set `runtimeSupported=true` or advertise ODST
  capabilities yet.
- The private ON scope remains minimal camera/stereo/6DOF plus first-person
  camera coherence and title-exit/stock fallback. Controls/aim/reticle,
  HUD/VISR, scopes, pause, brightness, motion blur, weapons, bones, arms, VRIK,
  and gameplay patches remain disabled for ODST.
- Title activation is a 50 ms module poll, not an atomic transition event. In
  an ambiguous `Unknown` state, shared Halo 3 behavior requires a Halo 3 camera
  heartbeat newer than the title transition and less than 100 ms old; explicit
  ODST/private camera-only ownership fails closed.
- The implementation currently multiplies OpenXR position by `1 / 3.048`.
  Treat that only as the hypothesis for the first calibration test, not accepted
  ODST scale evidence.
- The first private candidate from `bccf4c7` failed the initial headset smoke:
  menu VR controls stopped merging, ODST remained stock 2D at its camera-
  readiness wait, and Halo 3 performance regressed. The dedicated restore mode
  restored the exact headset baseline; the sealed recovery record remains
  preserved.
- The next isolated checkpoint restores only private frontend controller
  ambiguity, corrects the over-strict ODST readiness assumptions with one-shot
  diagnostics, and removes the measured Halo 3 reticle/palette hot paths.
  Clean option-OFF/ON Release builds, both CTest suites, and private wrapper
  unwind entries pass. No replacement candidate has been deployed.
- `deploy-odst-private.bat` remains the sole private path: it is token-gated,
  requires the reviewed branch/descendant, exact x64 OFF/ON caches and retail
  ODST hash, rebuilds/tests both configurations, preserves the exact installed
  headset baseline, deploys/restores only the byte-verified DLL, leaves the
  launcher untouched, reports hashes, and never launches MCC. Public scripts
  remain OFF-only.
- Bring-up order after headset acceptance of this candidate:
  controls/aim/reticle; ODST weapon and arm/VRIK calibration; HUD/VISR; scopes,
  vehicles, turrets, cutscenes, death/respawn, mission transitions, long
  sessions, and performance.
- Set ODST `runtimeSupported=true` only after Release/CTest, verified deployment,
  the complete Halo 3 shared-system regression, and ODST headset acceptance all
  pass. A missing or ambiguous ODST signature must leave the stock game running.

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
- Installer scripts were removed on 2026-07-20; install/update/uninstall are all
  manual folder operations. A real tester must still exercise copying a new
  build over an old one and confirm `halomccvr.cfg` survives it.
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
