# Stage 3CL - layout-relative capture bias (the reticle restoration)

**DLL:** `built/Stage3CL-HaloMCCVR.dll`
**SHA-256:** `4e23974c08697779153b98782f7ee5d4b79cf5ef7ffc922ad71213ee29b9df1b`
**Chain:** 3CI `fea7add6` (= 3BT capture chain + live un-hide + full
cutscene/theatre stack incl. Codex camera) -> 3CL. Installed both editions
2026-08-30 night; 3CK preserved under `-pre-3CL/`.

## What it fixes

The reticle capture went blank whenever the engine reported a layout other
than the one it was calibrated in. Session forensics (Halo 4 lines only -
an ODST line earlier produced a false lead):
- art 97..1492 in every session reporting `native base -1033.578/336.549`
- art 0 in every session reporting -1304.277/675.936 or -1893.000/1064.517
- the value is constant within a session (minutes of play, weapon swaps
  included) and differs between sessions; -1893.000 = backbuffer/2.

Derived from the accepted capture itself (reticle 181px at 2.5x = the
authored +/-37-unit square; the 3BK "104 px low" nudge):
- W = 4|baseX| = the published hide (0x2A8368) - keeps the accepted size.
- The frozen vertical nudge 104/1346.196 = 0.077255 was measured at offset
  ratio baseY/|baseX| = 0.325615. The offset scales with that ratio, so the
  failing sessions (ratios 0.5182/0.5624) put the reticle outside the
  2.5x window: byte-empty dumps, every weapon.
- Live rule: biasFraction = 0.237258 * baseY/|baseX| =
  0.949030 * baseY / hide -> reproduces 0.077255 EXACTLY at the calibrated
  layout, tracks any other.

## The change

Gate call 0x2FB992 -> `s3cl_layout_viewport` (228 B, in the dead 3BN region
0x2F93E0, prior bytes sha-pinned): W/TLx from live hide, H = W*bbH/bbW,
TLy = (512-H)/2 - H*0.949030*baseY/hide, then the intact 3BR scale tail
(2.5x zoom + RSSetViewports, 3BR consts/buffer reused). Any NaN/zero/
unpublished value falls back to the complete accepted 3BR thunk. Everything
else byte-identical to 3CI (which the layout-invariant experiment 3CK never
altered - 3CK is retired untested).

## What Stage 3CL contains, complete

- The FULL reticle/crosshair chain, byte-identical to accepted 3BT, plus
  live un-hide (Codex 3CD's correct byte) and this live framing.
- 3BU scene write-back (desktop mirror + damage vignette).
- 3BV/3BW/3BX cutscene detector, publications, capability (0x1F3), ODST
  look-rule.
- Codex's theatre camera (offsets verified correct against the snapshot
  struct).

## Open question (for the record)

WHAT flips the reported layout between sessions is still unproven (config,
FOV calibration and HUD bridge were identical across working and failing
logs; -1893 = half the 3786 backbuffer suggests a UI-scale mode). With the
framing live it no longer needs to be known for the crosshair to work; it
remains worth understanding.
