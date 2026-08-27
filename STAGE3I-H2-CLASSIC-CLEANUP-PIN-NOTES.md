# Stage 3I — Halo 2 Classic teardown pin

Status: **unaccepted headset test candidate** built from the exact Stage 3H DLL.

## Evidence

The Stage 3H headset log shows Halo 2 Classic working first, then level liveness closes and the Classic core attempts to remove its MinHook patches. Both `MH_DisableHook` calls return status `10`, which MinHook defines as `MH_ERROR_MEMORY_PROTECT`. The core correctly refuses to discard hook bookkeeping after that failure, but this leaves it permanently in `CleanupRequired`; later H2 generations repeatedly retry the same stale cleanup and Classic never installs again. Anniversary uses a separate stereo core and can therefore return, explaining the user-visible asymmetry.

## One behavioral change

Only the Halo 2 **Classic stereo-core teardown lifetime** changes:

- immediately before Classic teardown touches its MinHook targets, Stage 3I acquires a normal owning reference to the exact `halo2.dll` mapping already recorded by the core;
- that temporary reference is retained if cleanup itself fails;
- after all Classic hook records are successfully removed, the temporary reference is released;
- normal gameplay still uses the original non-owning module identity, so MCC remains responsible for title-DLL lifetime outside the cleanup window.

No Halo 2 camera, eye math, HUD, crosshair, hands, aim, renderer-switch, or Anniversary-stereo code changes. No Halo 4 bytes or behavior from Stage 3H are changed.

## Headset acceptance

1. Start Halo 2 Classic and verify stereo + 6DOF.
2. Switch Classic -> Anniversary -> Classic several times.
3. Let a checkpoint/loading transition occur, then return to gameplay in Classic.
4. Confirm no random stock/flat screen flashes after the first complete stereo pair is live.
5. Save `HaloMCCVR.log`.

Pass evidence: after a liveness close, the log should show `Halo 2 stereo core removed (...)` instead of an endless `cleanup REQUIRED ... disable outer=10 inner=10` loop, and Classic should reinstall/submit exact-current stereo pairs on the next eligible generation.
