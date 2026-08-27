HaloMCCVR Stage 3AK — Halo 2 Classic first-person particle gate
===============================================================

HEADSET CANDIDATE. Do not promote until the visible Halo 2 Classic muzzle flash
is confirmed gone in-headset.

Base: exact accepted Stage 3AI DLL
SHA-256: 36232bc077d1ca4f5080bf514f93fb5b70f746ec3baaba5e75c46a66e3a2d0a8

What changed:
- Halo 2 Classic only: current-user first-person particle dispatches are suppressed
  at the pinned retail particle renderer.
- Halo 2 Anniversary remains stock at the same renderer via its live gate byte.
- The hook is installed from the exact successful H2 module-publication edge, not
  Stage 3AJ's generic title-query edge that never armed in the failed headset run.
- Halo 4 Stage 3AI C50 full-coverage hide and every other title path are preserved.

First test:
1. Start Halo 2 in Classic graphics.
2. Fire BR/SMG/Magnum, then at least one Covenant weapon.
3. Confirm the log contains "Stage 3AK ACTIVE" and, after firing, one "Stage 3AK HIT".
4. Switch to Anniversary and verify its weapon effects remain exactly as Stage 3AI.
5. Switch back to Classic and confirm Classic suppression remains active.

If the visual flash remains but "Stage 3AK HIT" is present, save the log: that would
conclusively prove the visible flash is outside this current-user particle dispatch.
