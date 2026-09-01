# Stage 3CX — Halo 4 native reticle restored, layout-live (ACCEPTED BASELINE)

Status: **ACCEPTED by the user 2026-08-31** — "This is our new baseline."

- Input Stage 3CR SHA-256: `7D8E3EE6…C4C93878`
- Output Stage 3CX SHA-256:
  `7FDF539A91F36D030EEBB3DF24D162CDDE360AC950048E7B223D84807ED76D88`
- 2,923,520 bytes, 12 PE sections. Installed to BOTH editions.
- Builder: `tools/build_stage3cx_h4_three_point_bias.py` (self-verifying:
  anchors, monotonicity, embedded constants, payload disassembly).

## The defect this closes

Halo 4 re-picks its HUD layout class per level load. Observed gameplay
half-extent pairs (published live at 0x2A8360/64, hide = 4|x| at 0x2A8368):

| layout (|x| / |y|) | first seen | note |
|---|---|---|
| 1033.578 / 336.549 | all working 3BT/3BU-era sessions | the pair every frozen constant was calibrated on |
| 1304.277 / 675.936, 1344.409 / 697.226, 1466.886 / 654.646, 1234.047, 862.829 / 441.742 | blank evenings | capture byte-empty |
| 1635.814 / 731.878 | 2026-08-31 morning | black square, then restoration |
| 1308.806 / 582.373 | 2026-08-31 afternoon | third measured anchor |

The capture chain itself (3BU) was never lost — full byte accounting proved
every stage 3BU→3CB was additive. Three NUMBERS were frozen at the calibrated
layout: the selector un-hide distance, the vertical matte bias, and the zoom.

## What 3CX computes live (payload at 0x2F93E0, gate 0x2FB992 → payload →
## byte-identical accepted 3BR thunk 0x2F9A90)

- Selector un-hide (since 3CR): displacement at 0x2F99D7+4 → live 0x2A8368.
- `KBIAS` (0x2F9BD8) = clamp(P + Q·u + R·u², 0, 0.0772547), u = 1/|baseY|,
  P=0.0639733 Q=−86.04888 R=30463.99. Anchors, each measured from on-disk
  capture dumps: f(336.549)=0.0772547 (bit-exact accepted constant),
  f(731.878)=0.003274, f(582.373)=0.006040. NOT fixed-fraction, NOT
  fixed-units, NOT linear in 1/|y| — all three disproven by measurement.
- `KSCALE` (0x2F9BE0) = 0.101926 + 9914.386/hide. z(4134.312)=2.5000
  (accepted), z(6543.256)=1.61713 (=2.5×185/286, the measured size match).
- Invalid live values skip that refresh (frozen accepted constants remain).

Final verification dumps (layout 582.373): ink bbox x 46..465, y 93..427,
centroid (255,260) — full envelope in frame, 85/84 px vertical headroom,
horizontally exact. Accepted-look reference (3BT session): 185×174 centred
(254,271); reference bloom 432px.

## Maintenance recipe (any future layout that clips/misplaces)

1. The first four captures of each session auto-save as
   `Halo_MCC_VR/HaloMCCVR-h4-reticle-N.raw` (512×512 RGBA8, pitch 2048).
2. Measure ink bbox + centroid; compute the needed fraction from the miss
   (worked examples in the 3CU/3CW/3CX build-script docstrings).
3. Add `(|baseY|, fraction)` to `ANCHORS` in the 3CX build script; rebuild
   from the 3CR image (`built/Stage3CR-HaloMCCVR.dll`); the script re-solves
   the quadratic, re-verifies every anchor and monotonicity, and disassembles
   its own payload. Install via the standard candidate flow.

## Deferred (user: "we'll fix those bugs later")

1. Reticle animation cadence — `crosshair_animation_frames` set to **6** (the
   floor) in BOTH editions' cfgs at the user's explicit request 2026-08-31
   (prior cfgs under `out/deploy-backups/2026-08-31-cfg-anim6/`). UNTESTED at
   6. Next session: user judges the feel; measure real upload cost from the
   `timing:` lines before considering a floor patch below 6
   (`kMinimumUploadGapFrames`; acquire/wait once measured ~4-5 ms).
2. Theatre cine-bar colors — one cutscene showed white (top) / teal (bottom)
   where bars should be black. Investigation state: the theatre matte paints
   bars black only when active; `ComputeCutsceneTheaterMatte` is inactive
   when sourceAspect ≥ targetAspect (user cfg matte 1.76 vs H4 raster aspect
   1.387 → matte ACTIVE, so the mismatch is elsewhere — likely H4 letterboxes
   this cutscene internally at a different aspect, junk rows outside the
   matte window). User said "disregard" — parked, do not resume unasked.
3. ~15–20 s gameplay stutter — fps dips to 50, runtime cadence flapping
   144↔72 Hz; not capture-cadenced (uploads were 1 per 60 frames when
   reported). Needs a real profile.
4. Older parked: pre-rendered bink videos black; muzzle-FX suppression;
   stock vehicles.

## Session-facts worth keeping

- `pieces`/`held` in the upload log line are Reach-only; always 0 on H4.
- The `S3BH vp` "engine viewport" lines print OUR OWN pin; the first-frame
  4134×1346 is the calibrated-layout value because every session's first
  gameplay frames run that layout before the class switch.
- The capability flip 0xF3→0x1F3 (3BW) is bit-tested everywhere — benign.
- Codex's theatre camera hook (0x2FA36C) is gameplay-inert (verified in
  disassembly: non-theatre path returns untouched snapshot, al=1).
