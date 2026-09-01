# Stage 3BP - the Halo 4 capture keeps only the weapon reticle container

**DLL:** `built/Stage3BP-HaloMCCVR.dll`
**SHA-256:** `4298f4a7b9997b93123534d9c15906eeb94fb17a544a5192a1a63723765af0c5`
**Base:** Stage 3BO `129a24df...` (alpha for the reticle blit; not yet
headset-tested - the user asked for the whole chain to be finished offline
before testing).

## Why

The hide (and therefore the 3BI capture) moves every `reticule_offset_container`
push - three per gameplay pass: the weapon screen's, `damage_indicator`'s and
`grenade_indicator`'s. With 3BO the capture gains alpha, so the two indicators
would appear on the VR crosshair too, exactly as the damage arcs did under 3BL
(the 3BL run's `S3AS reticle probe` shows `alpha 456` for the 12 s the polyart
indicator was drawn - polyart writes alpha, bitmaps do not). The user wants the
reticle only.

## Discriminator, from the kit's own code

- `base_hud.cui_screen` instantiates `grenade_indicator` (template 0) and
  `damage_indicator` (template 1). Each container's only drawing children are
  `polyart_widget`s (`hud_grenade_indicator.polyart_asset`,
  `hud_hit_indicator.polyart_asset`).
- `assault_rifle.cui_screen`'s container holds `bitmap_widget`s (4x `ar_corner`
  quarter circles, 4x `white_pix` ticks) and `hit_indicator_art`; no polyart
  anywhere in the weapon screen.
- `halo4_tag_test.exe`, widget vtables resolved from RTTI (COL -> vtable, the
  method validated on `ReticuleOffsetContainerWidget` whose slot 27 is the
  known 0xADE020 that emits 0x28/0x29):
  - `PolyartWidget` slot 27 = 0xADA660 -> `0x9B60A0` allocates command
    **type 0x20** (payload 0xB0) / `0x9B6930` allocates **type 0x1F**
    (payload 0xF0, three-colour variant);
  - `c_bitmap_widget` slot 27 = 0xB02530 (jmp 0xB027B0) -> `0x9B9F50` type
    **0x0B**, `0x9B8D90` type **0x11** (material override), `0x9B96D0` type
    **0x0C**;
  - all through the same allocator `0x9B4600(buffer, type, size)` as the
    0x28 emitter, i.e. the same enum the retail executor switches on.

So a 0x20 or 0x1F command inside a 0x28 container identifies a polyart
container - an indicator - never the weapon reticle.

Retail numbering verified on `halo4.dll` (not assumed): the retail executor
`0x3F0EA4` dispatches by a compare tree; its type-0x20 case at `0x3F17F0`
reads payload `+0x10..0x1C`, `+0x20`, `+0x28`, `+0x60`, `+0xA0`, `+0xA8`
(pointer) exactly like the kit's 0x20 handler `0x9C52AF`, and its type-0x1F
case at `0x3F18A7` reads `+0x34..0x40`, `+0x28..0x30`, `+0x48..0x60`,
`+0xE0`, `+0xE8` exactly like the kit's `0x9C537B`. The kit's dispatch is a
0x31-entry jump table at `0x9C5804` (types 0..0x30), matching the emitter
table.

## The change (Halo 4 capture replay only)

The 3BI splice call at 0x53921 is re-pointed from the 3BI selector (0x2FBE40,
left in place) to `s3bp_capture_select` (0x2F98A0, 392 bytes, free tail of
the 3AS page after the 3BO HLSL text). It keeps every 3BI rule and adds:

- the 4-byte command header is read once, through `Halo4SafeRead`, BEFORE
  the original call (rdx/r8/r9 saved and restored around it);
- pre-call: type 0x20/0x1F while inside a container -> `poly := 1` and the
  transform-stack top is moved offscreen immediately (the proven hide shift),
  before the draw bakes its vertices;
- post-call: 0x28+0xC -> inside=1, poly=0; 0x29 -> both 0; then the 3BI
  enforcement with effective inside = inside && !poly, so the whole polyart
  container stays offscreen until its 0x29.

Visible pass, other titles, the hide itself: untouched.

## Expected headset result

The Halo 4 weapon reticle alone on the VR crosshair (AR: ring/arcs), blue,
red over enemies, changing with weapon swap; no damage arcs or grenade
indicator on the crosshair. If the dim inner ring seen in the 3BN dumps
disappears with this stage, it was indicator polyart; if it stays, it belongs
to the weapon screen (hit indicator art).

## Test

`python tools/test_stage3bp_h4_capture_weapon_only.py` - PASS: byte identity
outside the splice and payload, the whole selector decoded and checked
(SafeRead before the call and only under the guards, no raw [r13], argument
save/restore, result preservation, pre-call poly path, post-call
bookkeeping, enforce subroutine semantics and its exactly two stores, no
non-volatile register writes), 3BI/3BO/3BN artifacts, 3BJ absent.

## Deployment

Installed 2026-08-30 into both editions (Steam and Microsoft Store), each
hash-verified as `4298f4a7...`; 3BO preserved under
`out/deploy-backups/2026-08-30-pre-3BP/{steam,xbox}`.
