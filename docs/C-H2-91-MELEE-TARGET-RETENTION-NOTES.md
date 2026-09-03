# C-H2-91 Halo 2 controller-sight melee target retention - rejected

## Headset result - 2026-09-02

Rejected and disabled. Source
`34189bcef76ef49bd3c542af29e867a4c068ccd1`, Steam edition,
SteamVR/OpenXR 2.17.8, Oculus headset, 120 Hz. Log SHA-256:
`B154EB5BA323A5FC6E2974FAC1F8635AFAFFF91049FA892515E1545A83384DC8`.
The final combat telemetry reached `4,723` camera-suppressed owned calls,
`435` retained targets, and zero refusals. The user reports that melee still
missed and its lock-on/camera turn felt worse. Retaining the engine target
therefore restored the unwanted stock lunge behavior rather than making melee
follow the VR weapon. The behavior remains in source as dormant evidence and
must not be retried.

## Scope

This rejected candidate preserved the headset-confirmed C-H2-90 camera-assist fix and
makes one Halo-2-only change: retain the stock engine targeting result produced
from the already controller-owned unit sight. Halo 2 carries that identity into
`player_action.melee-target-unit` and its normal melee/lunge state.

The three camera-assist control floats remain zero. No melee range, damage,
hit volume, animation, character physics, weapon/map tag, game file, renderer,
reticle, projectile, HUD, hand, muzzle effect, or other title is changed.

## Evidence

- Source C-H2-90 installed the verified central calculation hook and logged
  `26,285` suppressed calls with zero refusals. The user reports the unwanted
  camera following stopped, while melee still misses.
- Official H2EK `player_control.cpp` consumes the three-float control result
  for camera/look adjustment but keeps a separate targeting block.
- Official H2EK `simulation_encoding.cpp` names the player-action field
  `melee-target-unit`; `unit_action_system.cpp`, `bipeds.cpp`, and
  `character_physics_mode_melee.cpp` carry that target into the stock lunge.
- The existing native-aim hook already makes the controller ray the owned
  unit's desired/current sight vector. No new retail binding is required.

See E-H2-78 in `docs/HALO2-SIGNATURE-EVIDENCE.md` for exact RVAs and lineage.

## Headset test

1. In Halo 2 Classic, stand just inside ordinary melee range and point the gun
   directly at one enemy while the headset faces slightly elsewhere.
2. Melee several times. The attack/lunge should choose the enemy under the gun
   sight instead of following the headset or missing sideways.
3. Slowly move the gun sight across enemies without touching camera rotation.
   The accepted C-H2-90 behavior must remain: the camera must not follow or get
   pulled toward them.
4. Confirm the native reticle and projectiles still follow the gun sight, then
   repeat briefly in Anniversary.
5. Attach `HaloMCCVR.log`. It must contain:

       Halo 2 aim-assist suppression Installed (C-H2-91)
       Halo 2 C-H2-91 aim-assist calculation: N calls, M camera assists suppressed ... (T target selected, ...)

   `M` must be non-zero. During close enemy testing, `T` should also become
   non-zero. `StockFallback`, zero suppressed calls, or zero selected targets
   means the intended path did not execute and is not a headset verdict.

This handoff is package-only: no automatic installation and no GitHub push.
