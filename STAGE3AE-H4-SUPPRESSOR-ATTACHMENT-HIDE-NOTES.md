# Stage 3AE — Halo 4 Suppressor / persistent first-person attachment-effect hide

Status: headset test candidate built directly from Stage 3AD.

## What the new evidence changes

The user-supplied H4 tag set separates the Suppressor (`storm_forerunner_smg`)
into two different visual paths:

1. its ordinary per-shot barrel firing effect, `fx\\fr_smg_firing_muzzle_flash`;
2. a weapon-object attachment effect, `fx\\fr_smg_firing`, driven by the
   weapon's `visable` scale.

The second path is the important Stage 3AE target. Its first-person event owns
three local particle/detail systems at non-primary authored locations:

- location 1 `mid_plate` -> `firing_mid_plate_1p`;
- location 2 `top_plate` -> `firing_top_plate_1p`;
- location 3 `fx_rear_slide` -> `firing_rear_slide_1p`.

All three are camera-mode 1 (first-person-only). The parallel third-person event
uses camera-mode 2 and remains stock.

This explains why Stage 3AD could make no visible difference on the Suppressor:
3AD targeted the sibling `gldf/lens/ltvl` non-particle dispatcher, while this
specific remaining Suppressor detail is the persistent weapon-attachment
particle/detail chain.

## Stage 3AE change

Stage 3AE leaves all Stage 3AD runtime patches in place, including:

- Stage 3Z `halo4.dll+0x27BD36` first-person particle deny (accepted primary
  muzzle-flash removal);
- Stage 3X/3AC `halo4.dll+0x1012D5` local-effect splice and finite XYZ hide;
- Stage 3AD `gldf/lens/ltvl` first-person non-particle dispatch suppression.

It changes only the retained Stage 3X local-effect wrapper inside HaloMCCVR.dll:

1. RVA `0x2EF72D` is tightened from Stage 3X's `camera != 2` admission back to
   exact `camera == 1`. Camera mode 0 and 2 are therefore excluded.
2. RVA `0x2EF736` replaces the old blanket negative-designator rejection with a
   guarded Stage 3AE gate in the verified mapped `.s3ic` zero cave at RVA `0x2F2007`.
3. The gate normally returns into the preserved Stage 3X finite-XYZ hide call.
   For a negative engine-special designator it admits only authored locations
   greater than zero. Location 0/negative-location special cases advance past
   that hide call and remain stock.

That extra `location > 0` condition is intentional: it captures the Suppressor's
persistent first-person plate/slide attachment rows without turning negative
primary-location effect cases into a global effect kill.

## Preserved

- Stage 3AD's H4-only non-particle dispatcher patch remains active for other
  Promethean weapons that actually use `gldf/lens/ltvl` first-person firing rows.
- Default-weapon primary muzzle particle suppression remains Stage 3Z unchanged.
- Halo 4 HUD, reticle, stereo, 6DOF, hands, two-hand, pause/re-entry and gun-stock
  behavior are byte-identical outside the two local-effect wrapper edits.
- Halo 2, Halo 3, ODST and Reach are untouched.
- PE section count, imports, headers, `.s3qd` live state and SizeOfImage are
  unchanged from Stage 3AD.

## Test focus

1. In Halo 4, verify the AR/default weapon muzzle flash is still absent.
2. Pick up the Suppressor and fire repeatedly. The detached/extra Promethean
   plate/hard-light weapon effect is the primary target and should now be gone.
3. If available, test other Promethean weapons; Stage 3AD's non-particle layer is
   retained, while Stage 3AE also catches camera-1 local non-primary attachment
   details with negative designators.
4. Confirm third-person/world/enemy/explosion effects remain normal.
5. Y+B pause/resume once, then Save & Quit/re-enter Halo 4 if convenient.
