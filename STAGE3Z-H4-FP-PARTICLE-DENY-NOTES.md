# Stage 3Z — Halo 4 first-person particle emission deny

Status: headset candidate built directly from the user-accepted Stage 3X all-title stability baseline.

## Base

- Exact Stage 3X DLL SHA-256:
  `486cc6e4f943c6fc58e227328777c54926799b1053f7e402767e3e38fca78ff6`
- Stage 3X remains the accepted compatibility baseline. Halo 2/H2A, Halo 3,
  ODST, Reach, Halo 4 lifecycle/pause/HUD/reticle/hands and gun-stock behavior
  are intentionally frozen.

## Why Stage 3Y was rejected

Stage 3Y restored the older loc0/local/camera1 matrix relocation and moved the
selected matrix out of play space. Headset testing showed the visible Halo 4
muzzle flash remained. That proves the visible particle survives downstream of
that transform path; Stage 3Z does not call the old +0x1012D5 installer.

## Proven Halo 4 particle boundary

The archived retail Halo 4 classifier at `halo4.dll+0x27BBCC` was already
traced by C-H4-D2. The caller return is `+0x27BE73`.

Retail control flow around camera mode:

- `+0x27BD1B`: test camera mode for 0
- `+0x27BD1F`: subtract 1
- `+0x27BD22`: jump to `+0x27BD36` only when the original camera mode was 1
- `+0x27BD36`: `41 8A DA` = `mov bl,r10b` (allow local FP-owned mode-1 particle)
- final return path loads EAX=5 and substitutes another bucket only if BL is true
- caller at `+0x27BE73` skips the particle row when EAX >= 5

C-H4-D2 captured the AR's local first-person systems as camera mode 1,
including its location-2 system. World/other systems observed as camera mode 0
or 2 travel different branches.

## Stage 3Z behavior

While the retained Halo 4 module reference is live, Stage 3Z exact-byte guards
`halo4.dll+0x27BD36` and changes only:

```
41 8A DA    mov bl,r10b
```

to:

```
30 DB 90    xor bl,bl ; nop
```

That forces Halo 4's own existing result-5 skip path for every camera-mode-1
particle system. Modes 0 and 2 never execute this site and remain stock.

This is deliberately broader than "AR loc0 only" but much narrower than the
historical `particle_render=0` fallback: first-person-only particles are denied;
world/third-person/independent particles stay on their original engine paths.
That includes the class used by muzzle and first-person Promethean weapon
particle/detail effects.

## Lifecycle safety

- The Stage 3X title wrapper is redirected to the Stage 3Z installer.
- The old Stage 3X `+0x1012D5` relocation installer is not called by the active
  title path.
- Successful H4 teardown restores `+0x27BD36` to the exact retail bytes before
  HUD/curvature cleanup and before the retained `halo4.dll` reference is freed.
- Unknown bytes or VirtualProtect failure return failure to the existing Stage
  3X fail-closed cleanup, retaining the module pin rather than touching unknown
  code.
- PE header/section layout and imports are unchanged from Stage 3X.

## Headset test

1. Enter Halo 4 and confirm normal Stage 3X stereo, hands, reticle and HUD.
2. Fire UNSC/Covenant weapons and several Promethean weapons.
3. Confirm local first-person muzzle/weapon particle effects are absent.
4. Confirm explosions, enemy/world fire and environmental particles remain.
5. Save & Quit, then re-enter Halo 4 once to confirm Stage 3X lifecycle remains intact.
