# C-H2-89 Halo 2 aim-assist-off test

Status: **rejected; disabled in source**.

This is one Halo-2-only optional change above C-H4-57. Halo 4 is unchanged.
The exact ordinary-Ghost-trail verification and C-H4-57 target result are
recorded in `docs/C-H4-57-GHOST-BOOST-BLACKOUT-NOTES.md`.

## Change

The official H2EK documents `sim_disable_aim_assist` as a boolean. C-H2-89
resolves that live retail debug-global slot by exact name and type 5, captures
its stock value, sets it to 1 only while the Halo 2 VR core owns a live level,
and restores the captured value on teardown. It reasserts the value from the
title worker if engine initialization resets it.

Any resolution or write failure leaves stock aim assist active and does not
affect camera, stereo, hands, HUD, reticle capture, weapons, or OpenXR. No game
files or tags are patched.

## Headset test

1. In Halo 2 Classic, aim near several enemies without moving the right stick.
   The view should no longer slow, follow, or pull toward them.
2. Point the controller sight line at enemies and fire; verify the native
   reticle and shots still follow the hand ray. Note whether target-sensitive
   red coloring changes—this global diagnostic may intentionally remove the
   target acquisition that drives it.
3. Melee an enemy from normal range and report whether the unwanted camera
   turn/miss is gone. Melee magnetism/lunge may be reduced because the requested
   test disables all engine aim assistance.
4. Switch to Anniversary once and confirm the same no-assist behavior without
   regressions to hands, HUD, reticle, muzzle effects, or stereo.
5. Attach `HaloMCCVR.log`. It must contain either the C-H2-89 `Installed` line
   naming `sim_disable_aim_assist`, or the explicit `StockFallback` reason.

This is a Halo-2-only engine-global lifecycle change, so it does not require a
new Halo 3 regression. The still-pending Halo 3 check belongs to carried-forward
C-H4-57 shared D3D behavior.

## Headset result (2026-09-02)

Source `03c7776f3118628c21f2faf0dfe3f9be3e3422e5` did not exercise the
intended change. The supplied Steam / SteamVR 2.17.7 / Oculus / 120 Hz log
reported `StockFallback` twice: retail's named debug-global record did not
resolve to a readable value slot. The player accordingly reported that aim
assist and melee behavior were unchanged. The runtime calls are now compiled
dormant; the implementation remains for evidence and is not deleted.
