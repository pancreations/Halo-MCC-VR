# C-H2-92 - controller-directed native Halo 2 melee targeting

## Why C-H2-91 failed

C-H2-91 retained a real native melee target, but official H2EK proves the
central aim-assist calculation selected that target from
`player_control->desired_angles` (the body/head camera), not from the unit aim
vector the VR mod already points with the controller. The stock lunge then
turned and moved toward that wrong target exactly as observed in the headset.

## Change

- Keep the accepted C-H2-90 removal of camera adhesion/friction.
- During only Halo 2's original central targeting calculation for VR-owned
  local user 0, replace the H2EK/retail player-control view-vector result with
  the same stable controller ray used by native weapon/unit aim.
- Let Halo 2 select and execute its native melee/lunge target along that ray.
- Leave every other call to the view helper stock.
- If the new helper is unavailable or cannot consume a coherent controller
  publication, keep C-H2-90's neutral no-target result for that calculation;
  never retain a body-camera target.

This does not alter melee range, damage, hit volumes, animations, body facing,
weapon/map tags, game files, Halo 2 Anniversary rendering, or another title.
The new helper is optional and cannot disarm camera, stereo, native aim,
hands, HUD, reticle, muzzle suppression, or OpenXR.

## Headset test

1. In Halo 2 Classic, face slightly beside an enemy while pointing the gun and
   native reticle directly at the enemy inside normal melee range.
2. Melee repeatedly. The native target/lunge should follow the gun ray and the
   hit should land without pulling toward the head/body-camera ray.
3. Move the gun across enemies without turning. The camera must retain the
   accepted C-H2-90 behavior and remain unpulled.
4. Briefly verify firing, the native reticle, Halo 2 Classic muzzle hiding,
   and Anniversary behavior remain unchanged.
5. Attach the log. It must report `controller melee targeting Installed
   (C-H2-92)`, non-zero central suppression, non-zero scoped view-helper
   controller overrides, and—during enemy tests—controller-ray targets.

`StockFallback`, zero controller overrides, or zero controller-ray targets
means the intended path was not exercised and is not a headset verdict.

Package only; no automatic installation and no GitHub push.
