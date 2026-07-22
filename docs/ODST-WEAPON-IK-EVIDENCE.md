# ODST weapon, arm-IK, two-hand, and dual-slot evidence

Status: private option-ON candidate, desk-side validated, headset proof pending.
Public ODST support remains disabled.

## Halo 3 behavior being matched

The accepted Halo 3 path is the player-facing specification:

- slot 0 is carried by the solved right hand and follows the right-controller
  aim pose;
- the right and left shoulder/elbow/wrist chains use the shared analytic IK
  solver, with the left controller owning the support hand;
- the same rigid hand transform drives the visible palette and the live
  marker/muzzle interpolation buffer;
- native flat-screen weapon IK is bypassed so shotgun/SMG support-hand
  animation cannot override the controller;
- two-hand acquisition, hold/toggle behavior, grip zones, and adjusted aim are
  owned by the existing OpenXR input path;
- slot 1, when the title supplies it, is carried by the solved left hand and
  follows the left controller;
- all behavior uses the existing universal F1/config values. No ODST-only
  setting or default is introduced.

## Official H3ODSTEK skeleton evidence

Source tag:

`objects\characters\odst_recon\fp\fp.render_model`

The official ODST first-person body contains 37 nodes:

| Role | Node | Parent |
|---|---:|---:|
| root | 0 | -1 |
| left upper arm | 1 | 0 |
| right upper arm | 2 | 0 |
| left forearm | 3 | 1 |
| right forearm | 4 | 2 |
| left hand | 5 | 3 |
| right hand | 6 | 4 |

The left hand/finger subtree is nodes 5, 7-11, 17-21, and 27-31. The right
hand/finger subtree is nodes 6, 12-16, 22-26, and 32-36. These are ODST tag
facts, not Halo 3 indices copied into the adapter.

Official ODST combined first-person animation graphs establish the append rule:

| Graph | Combined nodes | Weapon root | camera_control | Native weapon IK |
|---|---:|---:|---:|---|
| automag | 45 | 37 | 44 | none |
| shotgun | 43 | 37 | 42 | left_hand to left_hand |
| SMG | 42 | 37 | 41 | left_hand to left_hand |
| plasma pistol | 41 | 37 | 40 | none |

Every sampled graph appends the weapon subtree at node 37, parents it through
the authored right-hand carrier, and leaves `camera_control` as the final root
child. The adapter therefore computes:

- fixed ODST arm/hand masks from the official 37-node FP body;
- weapon nodes as 37 through `combined_count - 2`;
- `camera_control = combined_count - 1`, explicitly excluded from the
  carrier mask.

The earlier runtime probe captured a 46-node combined skeleton and a visible
palette mapping of weapon nodes 37-44, consistent with the official append
rule. Runtime counts outside 39-64 fail closed and remain stock.

## ODST hook and native-IK evidence

The title-specific interpolation boundary is the unique ODST signature at
retail RVA `0x1B3CB8`. The final visible-palette boundary is the unique shared
engine signature at ODST retail RVA `0x2EDD10`. Production resolves both by
unique AOB in the identity-checked `.text` section; neither RVA is hardcoded.

The exact retail bytes at the unique ODST native weapon-IK decision are:

```text
40 84 ED 74 05 45 84 FF 75 04 84 DB 74 0F BA 03 00 00 00
41 0F 28 D9 44 8D 42 FF EB 11
```

This is ODST's semantic counterpart to Halo 3's accepted decision. ODST uses
`xmm9` where Halo 3 uses `xmm8`; the branch structure and destinations are
otherwise exact. H3ODSTEK independently proves that the shotgun and SMG graphs
contain native support-hand weapon IK while the automag/plasma-pistol samples
do not.

The private adapter verifies the complete ODST byte sequence uniquely, then
changes only `74 05` to `EB 18`, selecting ODST's existing no-weapon-IK
branch. The original bytes are retained and restored after all seven ODST
detours reach verified quiescence. Any signature, byte, protection, hook, or
restore failure fails closed and retains the exact module reference for cleanup.

## Shared implementation boundary

ODST publishes its proven pre-head-look center position and post-head-look
camera basis to the existing controller-world transform. Its interpolation
hook builds an ODST-specific context, snapshots untouched bones, and gives the
live marker/muzzle buffer the same hand transform. Its palette hook calls the
existing Halo 3 `ReconstructVisiblePaletteSource` path. The ODST render hook
arms the same once-per-stereo-pair palette cache.

No Halo 3 TLS offset, tag-record layout, signature, sway callsite, bone lookup,
or cached render-player structure is used by ODST.

The existing universal values remain authoritative, including `arm_ik`,
`gun_scale`, `left_hand_scale`, gun pitch/yaw/roll/forward trims,
`left_hand_forward_m`, `two_handed_aim`, `two_hand_toggle`, both two-hand
zone trims, `left_grip_forward_m`, shoulder controls, and
`floating_hands`.

## Dual-wield boundary

The official ODST automag and SMG animation graphs still contain their
`first_person:dual:*` animation sets and dual pose/reload sounds. The render
adapter handles runtime slot 1 exactly like Halo 3: the solved left hand carries
the secondary weapon subtree and its marker/muzzle transform.

ODST campaign gameplay does not normally expose dual wield. The editing-kit
evidence above proves the animation/render data and slot-1 handling, but it does
not yet prove a safe retail gameplay gate that forces the campaign to create
slot 1. No unproven gameplay flag or Halo 3 offset is patched. If headset
testing never produces slot 1, enabling ODST campaign dual wield is a separate
evidence task, not something this candidate may claim as complete.

## Desk-side validation and required headset proof

Both configurations build in Release and pass the core suite:

- private ODST option ON: build plus `halomccvr_core_tests`;
- public/default option OFF: build plus `halomccvr_core_tests`.

Required headset acceptance before parity may be claimed:

1. automag, SMG, shotgun, and plasma pistol stay fused in stereo;
2. right gun follows the right controller with zero-trim barrel on the reticle;
3. both shoulders remain planted and both elbows bend naturally;
4. shotgun and SMG support hands leave their authored grips and follow the left
   controller;
5. two-hand hold and toggle modes match Halo 3 acquisition/release behavior;
6. muzzle flash and projectile origin stay on the visible gun through fire,
   reload, melee, weapon swap, death/respawn, and vehicle transitions;
7. any available slot-1/dual scenario follows the left controller and preserves
   both guns' effects;
8. F1 values and the same config file produce the same changes as Halo 3;
9. leaving ODST and entering Halo 3 passes a weapon/arm/two-hand regression
   check with no retained ODST branch or calibration state.
