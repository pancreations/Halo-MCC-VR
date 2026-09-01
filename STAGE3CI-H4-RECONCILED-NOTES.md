# Stage 3CI - reconciled build (our work + Codex's cutscene camera)

**DLL:** `built/Stage3CI-HaloMCCVR.dll`
**SHA-256:** `fea7add625c09eec37d44599b98d5690fcb9d840c5a0ac1766b69480dae627fd`
Installed both editions 2026-08-30 late night; 3CH preserved under
`out/deploy-backups/2026-08-30-pre-3CI/`.

## Contents

- Capture chain: **byte-identical to Stage 3BT** (the accepted crosshair
  build) across all seven regions - proven by
  `tools/test_stage3ch_h4_restore_capture_chain.py`.
- Plus one arithmetic correction: the 3BP selector's un-hide amount reads
  the **live** published hide displacement (0x2A8368) instead of the frozen
  4134.312. Without it, any layout whose hide differs leaves the reticle
  container off-screen, which is a guaranteed blank capture.
- Cutscene/theatre work retained in full: 3BU scene write-back (desktop
  mirror + damage), 3BV detector, 3BW publications + capability (0x1F3 in
  both masks), 3BX ODST look-constraint rule, and Codex's theatre-camera
  payload (offsets verified correct against `Halo4VrRenderSnapshot`).

## The finding that matters (2026-08-30 log forensics)

**Halo 4's reticle capture only ever worked for ONE authored layout.**
Filtering every preserved log to `Halo 4 reticle upload` lines:

| session | H4 art | reported `native base` |
|---|---|---|
| 10:22, 11:18, 15:52, 16:17, 16:34, 16:57, 17:28, 18:12 | 97..1492 | **-1033.578** |
| 19:25, 20:09, 20:31, 22:36, 23:25 | **0** | -1304.277 / -1893.000 |

4 x 1033.578 = **4134.312** = exactly the Stage 3BH framing constant and the
selector's un-hide amount. Every frozen calibration in the chain encodes that
one layout. When the game reports a different base (different weapon/level
authored reticle), the capture is blank.

Corollaries, all evidence-backed:
- Codex did **not** break the crosshair; its 3CD selector change was the
  correct half of the fix.
- The 3BU scene write-back did **not** break it (16:57 session: art 1492).
- The 19:25 "art 610" that briefly suggested otherwise was an **ODST** line,
  not Halo 4.
- Two dead ends this session, both disproven and not to be repeated:
  restoring the frozen bytes (3CH) and making the viewport width follow the
  live hide (3CE, which also resizes the window with the layout).

## Remaining work (one focused change, no probe needed)

Make the capture **framing** track the live layout the same way the un-hide
now does: derive the viewport from the published base/hide atomics
(baseX 0x2A8360, baseY 0x2A8364, hideX 0x2A8368) so the window reproduces the
accepted 16:34 geometry *as a ratio* rather than as constants. The accepted
geometry is `W = 4|baseX|`, `H = 4|baseY|` (Stage 3BH's own derivation),
then the 3BN raster-aspect/bias and 3BR 2.5x zoom on top. Stage 3CE tried
`W = live hide` but kept 3BN's `H = W*bbH/bbW`, which does not reproduce
3BH's height for a new layout - that is the piece to get right.
