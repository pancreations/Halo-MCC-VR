# Stage 3AH — Halo 4 Promethean camera-mode-2 particle contrast

This is a diagnostic headset build, not a claimed fix.

## Why this pass is different

The Stage 3AG log proved the +0x100EBC upstream-origin hook installed (`H4AF IN`) and
executed (`H4AF HIT`) while the orange Promethean effect stayed visible. That path is
therefore falsified.

The user's Promethean effect archive shows the Suppressor and Boltshot/stasis-pistol
chains repeatedly author the same first-person-named hard-light particle assets twice:
once as camera mode 1 and again as camera mode 2. Stage 3Z only denied mode 1.

Stage 3AH changes the already-proven retail particle classifier boundary, not an effect
transform. It preserves the Stage 3Z mode-1 deny and also denies the mode-2 allow write
at halo4.dll+0x27BD32. Mode 0 remains stock.

## Expected diagnostic result

- If the orange Suppressor/Boltshot hard-light effect disappears, the surviving family
  is proven to be the duplicated camera-mode-2 particle path. The next pass should narrow
  this from the intentionally broad contrast to Promethean/local weapon identities only.
- If it remains, camera-mode-2 particles are ruled out and we stop touching this classifier.

Because this test suppresses camera-mode-2 particles broadly in Halo 4, unrelated
third-person particle details may temporarily disappear. Do not treat that as final behavior.
