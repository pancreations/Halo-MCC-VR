# Halo 2 Classic / Halo 4 muzzle-flash tag index

Generated from the installed official mod tools on 2026-08-26.

Source tag roots:

- Halo 2 Classic: `D:\SteamLibrary\steamapps\common\H2EK\tags`
- Halo 4: `C:\Program Files (x86)\Steam\steamapps\common\H4EK\tags`

The CSV files beside this document are the exhaustive lookup tables:

- `muzzle_effect_chain.csv`: weapon -> firing effect -> particle/part -> render-model marker -> camera mode -> bitmap tag(s).
- `weapon_muzzle_marker_transforms.csv`: raw marker translations, quaternions, scale, and direction from first-person render models.
- `first_person_weapon_offsets.csv`: weapon-level first-person offsets and Halo 4 offset overrides.

## Halo 2 Classic: the important alignment detail

The Battle Rifle and Magnum do not put every visible flash component on one marker. Their firing effects use:

- Long flash particle: `effects\objects\weapons\rifle\battle_rifle\muzzle_flash.particle`
- Long flash bitmap: `effects\bitmaps\weapons_fire\br_muzzle_flash_long.bitmap`
- Long flash marker: `muzzle_flash` (both first- and third-person component)
- Center flash particle: `effects\objects\weapons\rifle\battle_rifle\muzzle_flash_center.particle`
- Center flash bitmap: `effects\bitmaps\weapons_fire\br_muzzle_flash_center.bitmap`
- Center flash marker: `primary_trigger` in first person
- Firing effect: `effects\objects\weapons\rifle\battle_rifle\fire_bullet.effect`

In `fp_battle_rifle.render_model`, the relevant raw marker transforms are:

| Marker | Instance | Translation | Quaternion |
|---|---:|---|---|
| `primary_trigger` | 0 | `0.191922, 0.000000, 0.045854` | `0, 0, 0, -1` |
| `muzzle_flash` | 0 | `0.172157, 0.000000, 0.042571` | `0, -0.461749, 0, -0.887011` |
| `muzzle_flash` | 1 | `0.172157, -0.002150, 0.046812` | `0.090424, 0.286788, 0.440377, -0.845957` |
| `muzzle_flash` | 2 | `0.172157, 0.002147, 0.046812` | `-0.090424, 0.286788, -0.440377, -0.845957` |

That split-marker setup is the first thing to audit in the VR port. A hook that redirects only `primary_trigger`, collapses the three `muzzle_flash` instances, or ignores their rotations will visibly detach the long flash from the gun.

Other Halo 2 Classic flash texture families:

| Weapons | Principal bitmap tag(s) | Principal marker(s) |
|---|---|---|
| Shotgun / Sniper Rifle | `effects\bitmaps\weapons_fire\muzzle_flash_center.bitmap`, `muzzle_flash_long.bitmap` | `primary_trigger`; Sniper also uses `muzzle_brake` for smoke |
| SMG | `effects\bitmaps\weapons_fire\fire_burst_round.bitmap`, `fire_burst_long.bitmap` | `primary_trigger` |
| Plasma Pistol / Plasma Rifle / Needler | `effects\bitmaps\energy\flash_c_generic_muzzle.bitmap` | `secondary_trigger`, `primary_trigger`, or `primary_trigger1`, depending on weapon/barrel |
| Covenant Carbine / Brute Shot | `effects\bitmaps\weapons_fire\muzzle_flash_long_2.bitmap` | `primary_trigger`; Carbine also uses `muzzle_break` for smoke |
| Fuel Rod / Flak Cannon | `effects\bitmaps\energy\flash_c_generic_big.bitmap`, `flash_c_generic_long.bitmap` | `primary_trigger` |
| Beam Rifle | `soft_flare_large`, `ring`, `burst_soft`, `electric_arcs`, and `energy_bolt_trail` under `effects\bitmaps` | `primary_trigger` plus `vent01`-`vent05` |

The Rocket Launcher firing tag contains smoke particles rather than a dedicated flash bitmap. Its weapon tag attaches `effects\generic_lights\muzzle_flash.light` at `primary_trigger`.

## Halo 4 Promethean weapon effects

The Promethean package includes the complete local `fx` branches for the six player weapons plus Bishop/Sentinel beam effect branches, their firing-effect dependencies, original `.bitmap` tags, and exported TGA/DDS textures.

| Weapon | Internal tag directory | Firing effect | Alignment-sensitive markers |
|---|---|---|---|
| Boltshot | `storm_stasis_pistol` | `fx\firing.effect`, `fx\charged_concussion.effect` | `primary_trigger`, `fx_vent`, `top_plate`, `bottom_plate`, `beam_origin` |
| LightRifle | `storm_forerunner_rifle` | `fx\homingrifle_firing.effect` | `primary_trigger`, `source_port` |
| Suppressor | `storm_forerunner_smg` | `fx\fr_smg_firing_muzzle_flash.effect` | **`primary_trigger_muzzle`**, `front_plate`, `fx_rear_slide`, `fx_vent_large` |
| Binary Rifle | `storm_forerunner_sniper_rifle` | `fx\fr_sniper_rifle_firing.effect` | `primary_trigger`, `secondary_trigger`, plates, cylinder, vent |
| Scattershot | `storm_spread_gun` | `fx\firing\sg_firing.effect` | `primary_trigger`, `secondary_trigger`, vent, rails, slides |
| Incineration Cannon | `storm_forerunner_incineration_launcher` | `fx\fr_incin_firing.effect` | `primary_trigger`, `secondary_trigger`, barrel, plates, vent |

Promethean signature texture paths live primarily under:

- `fx\bitmaps\2d\weapon_fire\fr_bolt_pistol`
- `fx\bitmaps\2d\weapon_fire\fr_rifle_hard_light`
- `fx\bitmaps\2d\weapon_fire\fr_sniper_rifle`
- `fx\bitmaps\2d\weapon_fire\fr_incineration_launcher`
- `fx\bitmaps\2d\weapon_fire\fr_muzzle_flash*.bitmap`
- `fx\bitmaps\gradient\grad_forerunner_muzzle*.bitmap`

Five bitmap paths referenced by Halo 4 particle tags are not present in this H4EK tag set:

- `fx\bitmaps\weapon_fire\gradients\muzzle_flare_carbine_long.bitmap`
- `fx\reach\bitmaps\weapon_fire\gradients\muzzle_flare_flak_cannon_long_soft.bitmap`
- `fx\reach\bitmaps\weapon_fire\gradients\muzzle_flare_magnum.bitmap`
- `fx\reach\bitmaps\weapon_fire\gradients\muzzle_flare_needler.bitmap`
- `fx\reach\bitmaps\weapon_fire\gradients\muzzle_flare_sniper.bitmap`

The original particle tags are included; these five absent paths are recorded here rather than silently substituted. The H4EK does contain local Magnum/Needler alternatives, but they are not the exact paths stored in those particle fields.

## Bundle layout

Each archive preserves tag-relative paths below `tags\`. Exported images are below `textures_tga\` (Halo 2) or `textures_exported\` (Halo 4). The `.bitmap` tags remain authoritative; exported TGA/DDS files are convenience copies for inspection.

