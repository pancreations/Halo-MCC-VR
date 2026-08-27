HaloMCCVR Stage 3AF — Halo 4 upstream first-person particle-origin hide
======================================================================

WHAT THIS TEST IS
-----------------
Stage 3AF is a targeted diagnostic/fix candidate for the remaining Halo 4
Promethean/Suppressor first-person weapon visual that survived Stage 3AD and
Stage 3AE.

Unlike the previous two passes, this does NOT guess another downstream effect
row. It restores the exact upstream halo4.dll+0x100EBC interception family from
the older V15.1 Promethean-visual work. That historical hook selected an engine-
special first-person particle-origin branch using the exact caller return,
first-person/local stack proof, and designator <= -2.

Stage 3AF keeps that historical classifier but changes the successful action:
returned BoneMatrix translations are moved to finite +10000 world coordinates
using the already-present Stage 3X finite-hide helper. The matrix stays valid;
only the selected visual origin is moved out of play space.

RUNTIME PROOF MARKERS
---------------------
This build deliberately logs its state so the next test is conclusive:

  H4AF IN   = exact halo4.dll+0x100EBC stock bytes matched and hook installed
  H4AF HIT  = the historical negative-designator FP-origin branch actually ran
              and its returned origin was hidden
  AFBAD     = exact runtime guard/install failed; no blind patch was applied

TEST
----
1. Install with MCC closed exactly as usual.
2. Launch Halo 4 and test the Suppressor/Promethean weapon that still shows the
   lingering effect.
3. Confirm normal/default weapon muzzle flashes remain suppressed as before.
4. Save HaloMCCVR.log and send it back even if the visual result is unchanged.

INTERPRETING THE RESULT
-----------------------
- Effect gone + H4AF HIT: this was the missing upstream path.
- Effect remains + H4AF HIT: the lingering image is not produced by this origin
  matrix; do not keep modifying this path.
- H4AF IN but no H4AF HIT: the old V15.1 classifier does not match the live
  lingering effect in this run.
- AFBAD: runtime bytes/protection did not match; the hook intentionally stayed
  off and the log tells us that directly.

PRESERVATION
------------
Stage 3AF is post-linked from the exact Stage 3AE DLL. Existing Halo 2, Halo 3,
ODST, Reach, Halo 4 stereo/6DOF/aim/hands/HUD/reticle, Stage 3Z default muzzle
suppression, Stage 3AD non-particle dispatcher suppression, and Stage 3AE bytes
remain otherwise unchanged. No claim of headset acceptance is made until this
specific test passes.
