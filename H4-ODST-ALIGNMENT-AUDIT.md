# Halo 4 vs ODST aim-alignment audit (2026-08-30)

Request: make Halo 4's gunshot / gun / crosshair alignment match ODST (the
one title a reviewer called properly aligned), and make sure the crosshair
can be turned off in the config so people can play without it. Then port to
the other titles.

Verdict up front: **on foot, the shipped Halo 4 build already implements the
exact ODST alignment contract, and the config off-switch already exists and
is correctly wired end-to-end (including through the 3A*/3B* binary
stages).** No behavioral change is required for either request; this audit
records the proof. The genuine remaining gaps vs ODST are listed at the end;
each needs its own evidence-first candidate.

## The alignment contract (how ODST does it)

1. **Crosshair rides the hand ray.** The OpenXR quad floats
   `crosshair_distance_m` along the (two-hand-adjusted, stabilized)
   controller aim ray. Shared code, all titles (`vr.cpp`, reticle quad).
2. **Bullets are steered through the crosshair point.** Halo spawns
   first-person projectiles at the ENGINE camera (the head), and no steering
   can move that origin. The shared aim author therefore aims the
   head-origin ray through the point the hand ray reaches at
   `crosshair_distance_m`; the closed-loop servo (XInput right stick)
   drives the engine's own aim onto that ray each frame
   (`game.cpp` ~38744, "Halo spawns first-person projectiles at the
   ENGINE's camera"). Seen from the eye, every shot lands exactly under the
   crosshair at every range; the ray and the hand ray coincide from the
   reticle outwards.
3. **The gun rides the hand.** ODST: solved arm IK + auto barrel alignment.
4. **Seated corrections.** ODST/Reach additionally hook
   `unit_get_camera_position` on the firing call site only and substitute
   the rendered eye, so a seated occupant's shot line IS the sight line
   (`OdstUnitCameraPositionHook`, game.cpp ~10345). This hook is gated to a
   SEATED player unit whose seat fires the personal weapon; it does not act
   on foot (on foot rendered eye == engine eye already).
5. **Honest reticle on rate-limited seats.** Turrets publish the engine's
   real aim to the quad (`Game_GetClampedAimDirection`).

## What Halo 4 already has (proof)

- **Same servo, same desired-ray computation.** `Game_ComputeAimStick`
  computes the head-origin-through-reticle-point angles BEFORE any title
  branch; Halo 4 supplies its own references via `Halo4ReadAimReferences`
  (C-H4-10, game.cpp ~38764) and closes the loop on the observer camera
  forward — "That IS the ray Halo 4 spawns first-person shots along"
  (game.cpp ~38872). Feedback is captured from the engine's stock camera
  before our per-eye overwrite (`stock.forward` publication, ~33571).
- **Log proof it tracks:** the 16:17–16:21 Steam campaign session
  (`HaloMCCVR.log.prev`, Stage 3BQ-era) has 40 `C-H4-10 look/aim: MOTION
  AIM` windows, **all with error 0.00 deg**; the only nonzero errors of the
  day (8.8 deg, 16:35:12) occur exactly at "aim steering blocked: Halo pause
  presentation active" — the pause, when the engine stops consuming aim.
- **The gun rides the hand:** C-H4-43/44 palette lines during gameplay show
  e.g. `1492 Storm hand palettes committed / 0 refused, 1492 held records
  committed / 0 refused` per 2 s. The held weapon and hands are
  controller-carried every frame (rigid, no IK — C-H4-34 replaced the
  rejected arm solve).
- **On-foot origin:** Halo 4 vehicle state is never consulted (`halo4Aim`
  skips every Halo 3 seat block), and the ODST-style firing-origin hook is
  unnecessary on foot for the same reason it is a no-op on foot in ODST.

Conclusion: mathematically, Halo 4 on foot = ODST on foot, today.

## The crosshair off-switch (config)

Already present and correctly gated for Halo 4:

- `crosshair = 0` (or F1 -> "Show a crosshair where the weapon shoots"
  unticked): the VR quad is not submitted (`reticleQuadSubmitted` requires
  `g_config.crosshair`, vr.cpp ~10122) AND the native flat reticle stays
  hidden (`Halo4DecideCuiReticleAction`: `!crosshairEnabled ->
  HideNative`). Result: NO reticle anywhere — pure gun-barrel aiming. The
  aim servo keeps steering bullets onto the hand ray regardless; the
  crosshair is presentation only.
- `kill_reticle = 0` (config file only, no F1 entry): explicit request for
  the stock face-centred CUI reticle — native drawn (`DrawStock`), quad
  suppressed for H4 (vr.cpp ~10131 "Never add a held authored gun-ray quad
  on top of it").
- Default (`crosshair = 1`, `kill_reticle = 1`): native hidden, authored
  pixels on the quad (current behavior).
- **The 3A*/3B* binary stages are inert when the capture is off**: the whole
  capture/replay (and therefore the 3BI/3BP selector splice at 0x53921, the
  3BN/3BR viewport gate, 3BM/3BT dumps, 3BO blit, 3BQ RTV, 3BS identity)
  hangs off `wantsCaptureReplay = ... && g_config.crosshair &&
  g_config.kill_reticle && VR_ShouldCaptureAuthoredReticleThisFrame()`
  (game.cpp ~32918). The 16:35 log confirms the ratio: 4 authored captures
  vs 1455 native hides per 2 s.

## Capture content check (rolling dumps, 16:34 run)

All four rolling snapshots (Stage 3BT) from the 16:34–16:35 session hold
ONLY the reticle: ring + four quarter-circle petals, 9286 ink texels, bbox
181x171 px centred (the 2.5x Stage 3BR scale, as designed). No damage
indicator, no extra elements. Converted copies preserved in the session
scratchpad (`run-1635/`).

## Genuine remaining gaps vs ODST (each needs its own candidate)

1. **Muzzle flash / local FP effects are suppressed, not carried.** The
   Stage 3AC/3AD/3AH/3AI/3AZ chain ("combined muzzle hide", "fp particle
   deny", "C50 full-coverage hide") moves local first-person weapon effects
   finite-far because stock they render at the engine's (face-anchored)
   viewmodel. ODST re-registers markers/muzzle onto the carried gun instead.
   Re-attaching H4 effects to the held model would need the effect-location
   matrix rewritten with the held-model transform — C50 once had a "reroot
   policy" that was replaced by suppression, so this must be re-approached
   with evidence, not by turning the old path back on.
2. **Vehicles/turrets are stock.** "Vehicle seat/camera/projectile
   ownership ... NOT OBSERVABLE FROM CURRENT PROVEN HOOKS" (H4DIAG, every
   window). No seat detection => no turret honest-reticle, no seated
   firing-origin substitution, and the hand-ray quad keeps floating in
   vehicles even though vehicle weapons ignore the hand. This is the H4EK
   evidence project the parity diagnostic already scopes.
3. **Halo 3 (for the later port): no firing-origin hook.** H3 uses the O5
   seat re-aim approximation (crossing at reticle distance) instead of
   ODST/Reach's exact eye substitution ("HALO 3 HAS NO SUCH HOOK",
   game.cpp ~38846). Porting ODST's hook to H3 is the main "other halos"
   alignment item; Reach already has its own (R-V10, two call sites).

## Headset acceptance test (plain language)

On foot in Halo 4, either edition:

1. Fire at something FAR (20 m+): shots land under the crosshair.
2. Fire at something NEAR (2–5 m): shots land under the crosshair as seen
   from your eyes (the barrel line differs slightly near — same as ODST).
3. F1 -> untick "Show a crosshair where the weapon shoots": both the VR
   crosshair and the game's flat reticle disappear; bullets still follow
   the hand. Aim down the gun.
4. (Optional, config file) `kill_reticle = 0`: the stock flat Halo 4
   reticle returns, no VR crosshair.

Report which edition. ODST-style vehicle alignment and muzzle flash are
explicitly NOT claimed by this audit.
