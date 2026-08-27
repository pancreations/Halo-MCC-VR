# Stage 3AD — Halo 4 first-person non-particle weapon-effect hide

Status: headset test candidate built directly from **Stage 3AC**, the current
working H4 anchor after the rejected 3AA/3AB black-screen candidates.

## Base held fixed

Stage 3AD does not change Stage 3AC's accepted/proven systems:

- Stage 3Z camera-mode-1 particle deny remains intact. This is the headset-tested
  change that removed the primary Halo 4 muzzle particle.
- Stage 3X local-FP residual matrix suppressor at `halo4.dll+0x1012D5` remains
  intact.
- Halo 4 HUD size/aspect/height/curvature, procedural bullet-ray reticle,
  B-edge pause restore, real camera heartbeat, module lifetime, Save & Quit
  cleanup/re-entry, hands, two-hand, and gun-stock behavior are unchanged.
- Halo 2, Halo 3, ODST, and Reach are unchanged.
- Q-R1 loader geometry is unchanged: 12 sections, final `.s3qd` at `0x2F3000`,
  `SizeOfImage=0x2F4000`.
- The live `.s3qd` state prefix `0x2F3000..0x2F31FF` is byte-identical to Stage
  3AC. Stage 3AD never uses that area as executable storage.

## Why a second H4 effect family is required

The supplied H4 firing-effect table shows that Promethean weapon firing effects
are not particle-only. In addition to ordinary particle systems, the firing
effects contain `gldf`, `lens`, and `ltvl` components. Examples include:

- `storm_spread_gun`: `gldf` `muzzle_flash` and `ltvl`
  `firing_forward_flash_1p`;
- `storm_forerunner_rifle`: first-person lens flare/muzzle flare entries plus
  `ltvl` muzzle-glow data;
- `storm_stasis_pistol`: first-person `gldf` muzzle-flash entries;
- `storm_forerunner_incineration_launcher`: `gldf` muzzle-flash data.

Stage 3Z correctly suppresses the first-person particle class, but these
non-particle component groups are dispatched by a different retail H4 branch.

## Retail dispatcher proof and Stage 3AD changes

The pinned retail H4 image (`timestamp 0x68A0E7BF`, `SizeOfImage 0x04A3F000`)
contains a single dispatcher around `halo4.dll+0x3A08xx..0x3A0Bxx` for these
non-particle groups.

Stage 3AD changes only three decisions at runtime while the retained H4 module
is live:

1. `halo4.dll+0x3A0822`
   - stock: `84 0D B8 2D D2 00`
   - Stage 3AD: `45 84 ED 90 90 90`
   - The stock `gldf` debug-global test becomes `test r13b,r13b`; the already
     existing `JNE` takes H4's stock skip path only when the render context is
     first-person.
2. `halo4.dll+0x3A0AA1`
   - stock: `84 C9`
   - Stage 3AD: `31 C9`
   - This instruction is reached only from the `r13b != 0` LTVL path; the
     existing following `JE` therefore takes the stock skip. The non-FP LTVL
     path jumps around this instruction.
3. `halo4.dll+0x3A0B1A`
   - stock: `84 C9`
   - Stage 3AD: `31 C9`
   - H4 itself tests `r13b` immediately before this lens branch. The changed
     instruction makes the existing first-person skip unconditional; the
     `r13b == 0` path bypasses it and remains stock.

There is no new MinHook, no V11 transform bridge, no `particle_render=0`, and no
weapon-specific Scattershot special case. The intent is the whole first-person
non-particle firing family, including Promethean weapons.

All three edits share the `halo4.dll+0x3A0000` page. The helper uses one guarded
VirtualProtect transaction, restores the prior page protection and flushes the
instruction cache through Stage 3X's existing helper. The XOR write is
self-inverse, so install and teardown use the same exact transaction.

## Lifecycle / re-entry

The Stage 3AD cleanup wrapper restores these three retail bytes **before**
calling Stage 3AC's existing cleanup. If the Stage 3AD restore cannot prove a
known state or cannot acquire write protection, it returns failure; the existing
H4 lifetime code therefore retains the H4 module and can retry rather than
unloading modified code.

## Post-link build

Exact input DLL SHA-256:
`3a09288b5b8de4420ffc08695ebeb7431456971ccfe59de1f8c43f999caf700d`

Build command:

```text
python tools/build_stage3ad_h4_promethean_nonparticle_hide.py \
  baseline/Stage3AC-HaloMCCVR.dll HaloMCCVR.dll
```

The builder refuses every other input hash, guards all three executable gaps,
guards the two Stage 3AC call edges, preserves the Q-R1 PE geometry, and does
not modify MCC or `halo4.dll` on disk.
