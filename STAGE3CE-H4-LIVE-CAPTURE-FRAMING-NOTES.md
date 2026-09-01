# Stage 3CE - live Halo 4 capture framing (the crosshair-regression fix)

**Cumulative DLL:** `built/Stage3CE-HaloMCCVR.dll`
**SHA-256:** `f23fa778c3755c5f16e1a321ec9e70af488a484f2a8fd1acb4984ee08832dc98`
**Chain:** 3BX `54130fd5` (accepted line) -> 3CB `9ee60ca1` (Codex's proven
theatre-camera delta on 3BX) -> 3CE. One behavioral change: the capture
framing follows the live CUI layout.

## Reconciliation record (2026-08-30 late evening)

After the session limit, Codex continued the ladder: 3BY (judged failed),
3BZ/3CA/3CB (from 3BX), then 3CC (the same theatre-camera delta rebased onto
3BW, dropping the accepted 3BX look-constraint rule) and 3CD (a partial
crosshair fix). 3CD was installed and the user reported: 3D cutscenes work,
the crosshair is broken.

Log forensics across the preserved sessions:
- The theatre camera delta (a hook on `VR_Halo4GetRenderSnapshot` 0x2EA60 at
  callsite 0x56EBF: during theatre, eye offsets scaled by the Depth config,
  eye orientations forced parallel, fov/head-pose valid flags cleared so the
  authored camera and its FOV own the capture) worked in every session
  (19:25, 20:09, 20:31: entered/exited theatre cleanly with cine bars).
- The crosshair was ALREADY blank in the 19:25 session (pieces 0, art 0,
  blankHeld ~200), so 3CD was an attempted fix, not the cause - and 20:31
  proves it did not work.
- Root cause: the whole capture-framing chain (3BH constant, 3BE pin, 3BK/
  3BN/3BR gate thunks, 3BP selector) was calibrated to ONE session's CUI
  layout. The 16:34 accepted session: native base -1033.578/336.549, hide
  4134.312 (exactly the 3BH constant). Tonight's sessions: base
  -1304.277/675.936, hide 5217.107. The selector un-hid by 826 units too
  little and the pinned viewport framed the old position -> every capture
  blank -> the shared procedural placeholder instead of native art.

## The change (three byte-regions, one behavior)

1. The gate call at 0x2FB992 lands on `s3ce_live_viewport` (201 bytes,
   REPLACING THE DEAD 3BN THUNK at 0x2F93E0..0x2F94D0 - unreachable since
   3BQ, zero inbound references, prior bytes pinned by SHA-256
   `e4e61358...` before overwrite). The thunk builds the private viewport
   from the LIVE hide displacement (0x2A8368, a float the native-hide path
   publishes at 0x53E04 on every gameplay CUI pass):
   `W = hideX_live; TLx = (512-W)/2; H = W*bbH/bbW;`
   `TLy = (512-H)/2 - H*(104/1346.196)` - then jumps into the intact 3BR
   scale tail (0x2F9B29) for the accepted 2.5x zoom about (256,256) and the
   RSSetViewports call, reusing 3BR's own constants and buffer. NaN, <=0, or
   zero backbuffer falls back to the complete old 3BR thunk (0x2F9A90).
2. The 3BP selector's un-hide load at 0x2F99D7 now reads the same live value
   (the one byte-region Codex's 3CD proved; the 0.5x discrimination
   threshold scales with it automatically).
3. Everything else byte-identical to 3CB: the 3BX detector chain
   (jne 0x2C2BC -> 0x2FACB0 -> 3BU write-back 0x2F9D10), the theatre-camera
   payload at 0x2FA36C, both capability masks, all prior artifacts.

Codex's 3CC/3CD line (built on 3BW, missing 3BX) is retired; its builds
remain under `built/` for the record. Codex also edited
`src/dll/game.cpp`, `src/dll/vr.h`, `src/common/cutscene_theater_logic.h` -
those edits do not affect post-link candidates and are left for review.

## Expected result

- The native Halo 4 crosshair art returns in gameplay under ANY HUD layout
  (the framing tracks the engine's own published values; a layout change
  costs at most one CUI pass of warmup after a level load).
- 3D cutscene theatre exactly as the user accepted it tonight: locked
  authored camera, authored FOV, Depth slider live, ODST look-rule (a
  look-free cinematic stays immersive), theatre config/F1 checkbox working.
- Other titles and all prior Halo 4 behavior unchanged.

## Test

`python tools/test_stage3ce_h4_live_capture_framing.py` - PASS (byte
identity outside the three regions; thunk decode incl. guards, live reads,
buffer-only writes, both exits; selector neighbors byte-identical; dead-3BN
base region pinned by hash; 3CB/3BX/3BU and all prior artifacts intact).

## Deployment

Installed 2026-08-30 late evening into both editions (MCC confirmed
closed); the Codex 3CD install preserved with its logs/cfg under
`out/deploy-backups/2026-08-30-pre-3CE/{steam,xbox}`.

## Headset test (plain language)

1. Play Halo 4 gameplay: the proper weapon crosshair (the game's own art)
   should be back on the VR crosshair - blue, red over enemies, changing
   with weapons.
2. Let an in-engine cutscene play: the 3D theatre screen as you accepted it
   tonight (locked camera, no head-follow, Depth slider works).
3. If you changed HUD size/aspect sliders, the crosshair should still work -
   that is the point of this fix.
4. Say which edition you used.
